#pragma once
// ENVIRONMENT LIGHTING (image-based lighting) - the GLSL side every lit material shares, plus the bindings RenderSystem
// feeds it. A material opts in with
//
//     #include "engine/env_lighting.glsl"
//     ...
//     color += EnvLighting(vWorldPos, N, normalize(vN), V, albedo, F0, perceptualRoughness, metallic, ao);
//
// (ShaderLoader expands the #include; see ShaderLoader::expandIncludes.) RenderSystem_Environment.cpp builds what it
// samples, the split-sum approximation of Karis 2013 ("Real Shading in Unreal Engine 4"):
//
//   * a cube map ARRAY of prefiltered radiance: layer 0 = the sky (EnvironmentSkyComponent), layers 1..N = reflection
//     probe captures (ReflectionProbeComponent). Mip m holds the radiance convolved with the GGX lobe of perceptual
//     roughness m / (mips - 1), so a rough surface just reads a blurrier mip;
//   * a 2D LUT of the GGX BRDF integrated over the hemisphere (scale A and bias B applied to F0) for (NdotV, roughness);
//   * 9 spherical harmonics coefficients per layer for the diffuse irradiance (Ramamoorthi & Hanrahan 2001), already
//     convolved with the cosine lobe and divided by pi: evaluating them gives the radiance a white Lambert surface
//     reflects, so diffuse = albedo * SH(N).
//
// Probes blend by how deep the point is inside their box (fading over blendDistance at the faces); what is left of
// the weight goes to the sky. Box probes are parallax corrected (Lagarde & Zanuttini 2012): the reflection ray is
// intersected with the box and the probe is sampled toward that hit, as if the captured scene lay on the box faces.

namespace EnvLighting
{
    inline constexpr const char* IncludeKey = "engine/env_lighting.glsl";

    // Texture units and SSBO binding. Units 0-9 are the PBR maps, shadows and SSAO (see DefaultShader.hpp).
    inline constexpr int kPrefilteredUnit = 10;   // samplerCubeArray uEnvPrefiltered
    inline constexpr int kBrdfLutUnit = 11;       // sampler2D        uEnvBrdfLUT
    inline constexpr int kProbeBinding = 3;       // SSBO EnvProbeBuffer (bindings 0/1 = point lights/shadows)

    // Layer 0 is the sky, so the array holds up to kMaxProbes + 1 layers.
    inline constexpr int kMaxProbes = 32;

    inline constexpr const char* Source = R"GLSL(
// ---- Environment lighting (engine/env_lighting.glsl, see EnvLighting.hpp) ----
uniform samplerCubeArray uEnvPrefiltered;  // unit 10 - prefiltered radiance, layer 0 = sky, 1..N = probes
uniform sampler2D        uEnvBrdfLUT;      // unit 11 - RG = split-sum scale/bias for (NdotV, perceptual roughness)
uniform int   uEnvEnabled;                 // 0: EnvLighting() returns black (setting off, or no map built yet)
uniform int   uEnvProbeCount;              // probes in the SSBO after the sky (layers 1..uEnvProbeCount)
uniform float uEnvMaxLod;                  // last mip = perceptual roughness 1
uniform float uEnvIntensity;               // the "intensity" setting: reflections and ambient alike
uniform int   uEnvDebugView;               // 1: the material outputs only the environment term

struct EnvProbe {
    vec4 sh[9];     // irradiance / pi, constants folded in: evaluate with the bare polynomials (EnvIrradiance)
    vec4 boxMin;       // xyz = influence box min, w = blend distance
    vec4 boxMax;       // xyz = influence box max, w = 1 when parallax corrected
    vec4 center;       // xyz = capture position, w = cube array layer
    vec4 parallaxMin;  // xyz = parallax proxy box
    vec4 parallaxMax;
};
layout(std430, binding = 3) readonly buffer EnvProbeBuffer { EnvProbe envProbes[]; };

vec3 EnvIrradiance(int i, vec3 n)
{
    vec4 r = envProbes[i].sh[0]
           + envProbes[i].sh[1] * n.y
           + envProbes[i].sh[2] * n.z
           + envProbes[i].sh[3] * n.x
           + envProbes[i].sh[4] * (n.x * n.y)
           + envProbes[i].sh[5] * (n.y * n.z)
           + envProbes[i].sh[6] * (3.0 * n.z * n.z - 1.0)
           + envProbes[i].sh[7] * (n.x * n.z)
           + envProbes[i].sh[8] * (n.x * n.x - n.y * n.y);
    // Order-2 SH rings slightly below zero opposite a very bright area (lava under a dark sky).
    return max(r.rgb, vec3(0.0));
}

// 1 well inside the box, fading to 0 at its faces over the blend distance; 0 outside.
float EnvProbeWeight(int i, vec3 P)
{
    vec3  d     = min(P - envProbes[i].boxMin.xyz, envProbes[i].boxMax.xyz - P);
    float inner = min(min(d.x, d.y), d.z);
    return clamp(inner / max(envProbes[i].boxMin.w, 1e-3), 0.0, 1.0);
}

// Direction to sample probe i with for a ray from P along R. A P outside the parallax box (the influence box can be
// larger) gets t clamped at 0: the probe is then sampled from P itself, still a sensible direction.
vec3 EnvProbeDirection(int i, vec3 P, vec3 R)
{
    if (envProbes[i].boxMax.w < 0.5) return R;
    // Exit point of the ray through the box: the nearest of the three far slab planes. 1e-6 keeps an axis-parallel
    // ray finite (its slab is then hit at +-inf and never chosen).
    vec3 invR = 1.0 / (R + vec3(1e-6) * (step(0.0, R) * 2.0 - 1.0));
    vec3 tA   = (envProbes[i].parallaxMax.xyz - P) * invR;
    vec3 tB   = (envProbes[i].parallaxMin.xyz - P) * invR;
    vec3 tFar = max(tA, tB);
    float t   = max(min(min(tFar.x, tFar.y), tFar.z), 0.0);
    return (P + R * t) - envProbes[i].center.xyz;
}

vec3 EnvRadiance(int i, vec3 dir, float lod)
{
    return textureLod(uEnvPrefiltered, vec4(dir, envProbes[i].center.w), lod).rgb;
}

// Ambient light + reflections from the environment for a surface point.
//   P: world position; N: shading normal; Ng: geometric normal (fades reflections that a normal map bends below the
//   surface); V: unit vector toward the eye; albedo/F0/metallic as in the direct lighting; perceptualRoughness
//   unsquared; ao: material AO x SSAO.
vec3 EnvLighting(vec3 P, vec3 N, vec3 Ng, vec3 V, vec3 albedo, vec3 F0,
                 float perceptualRoughness, float metallic, float ao)
{
    if (uEnvEnabled == 0) return vec3(0.0);

    float rough = clamp(perceptualRoughness, 0.0, 1.0);
    float alpha = rough * rough;
    float NdotV = clamp(dot(N, V), 1e-4, 1.0);

    // Off-specular peak: the GGX lobe's dominant direction leans toward the normal as roughness grows (Frostbite,
    // Lagarde & de Rousiers 2014). Keeps rough metals from looking like blurred mirrors at grazing angles.
    vec3  Rm = reflect(-V, N);
    vec3  R  = normalize(mix(N, Rm, (1.0 - alpha) * (sqrt(1.0 - alpha) + alpha)));
    float lod = rough * uEnvMaxLod;

    vec3  irradiance = vec3(0.0);
    vec3  radiance   = vec3(0.0);
    float wsum       = 0.0;
    for (int i = 1; i <= uEnvProbeCount; ++i)
    {
        float w = EnvProbeWeight(i, P);
        if (w <= 0.0) continue;
        irradiance += w * EnvIrradiance(i, N);
        radiance   += w * EnvRadiance(i, EnvProbeDirection(i, P, R), lod);
        wsum       += w;
    }
    // Overlapping probes: normalised. Partial coverage: the sky fills the rest.
    if (wsum > 1.0) { irradiance /= wsum; radiance /= wsum; wsum = 1.0; }
    float skyW = 1.0 - wsum;
    if (skyW > 0.0)
    {
        irradiance += skyW * EnvIrradiance(0, N);
        radiance   += skyW * EnvRadiance(0, R, lod);
    }

    // Split sum + multiple-scattering energy compensation (Fdez-Agueera 2019): single-scatter GGX loses energy on
    // rough surfaces (rough metals came out too dark); the lost share comes back as an extra, irradiance-lit lobe.
    vec2  ab     = texture(uEnvBrdfLUT, vec2(NdotV, rough)).rg;
    vec3  FssEss = F0 * ab.x + ab.y;
    float Ems    = 1.0 - (ab.x + ab.y);
    vec3  Favg   = F0 + (1.0 - F0) / 21.0;
    vec3  FmsEms = Ems * FssEss * Favg / (1.0 - Favg * Ems);
    vec3  kD     = albedo * (1.0 - metallic) * max(1.0 - FssEss - FmsEms, vec3(0.0));

    // Specular occlusion from the AO (Lagarde & de Rousiers 2014): cavities lose the reflection too, mostly at grazing
    // angles and on smooth surfaces. Horizon fade: a normal-mapped reflection pointing under the geometric surface
    // would sample what is behind it.
    float specAO  = clamp(pow(NdotV + ao, exp2(-16.0 * alpha - 1.0)) - 1.0 + ao, 0.0, 1.0);
    float horizon = clamp(1.0 + dot(Rm, Ng), 0.0, 1.0);
    horizon *= horizon;

    vec3 specular = (FssEss * radiance + FmsEms * irradiance) * specAO * horizon;
    vec3 diffuse  = kD * irradiance * ao;
    return (specular + diffuse) * uEnvIntensity;
}
// ---- end of environment lighting ----
)GLSL";
}
