#version 430 core

// -------------------------------------------------------
// Varyings from vertex shader
// -------------------------------------------------------
in vec3 vWorldPos;
in vec2 vUV;
in mat3 vTBN;

out vec4 FragColor;

// -------------------------------------------------------
// Texture units  (bound per-submesh by Mesh::render)
// -------------------------------------------------------
uniform sampler2D uAlbedoTex;    // unit 0 — baseColor  (sRGB)
uniform sampler2D uNormalTex;    // unit 1 — tangent-space normal
uniform sampler2D uMRTex;        // unit 2 — G=roughness, B=metallic
uniform sampler2D uOcclusionTex; // unit 3 — R=AO
uniform sampler2D uEmissiveTex;  // unit 4 — emissive (unused)

// -------------------------------------------------------
// Per-frame uniforms
// -------------------------------------------------------
uniform ivec2 uScreenSize;
uniform vec3  uCameraPos;

// -------------------------------------------------------
// Forward+ SSBOs
// -------------------------------------------------------
struct PointLight {
    vec4 posRadius;       // xyz = world pos,  w = radius (culling only)
    vec4 colorIntensity;  // rgb = color,       a = intensity (candelas)
};

struct TileData {
    uint offset;
    uint count;
};

layout(std430, binding = 0) readonly buffer LightBuffer { PointLight lights[]; };
layout(std430, binding = 1) readonly buffer LightIdx    { uint lightIndices[]; };
layout(std430, binding = 2) readonly buffer TileGrid    { TileData grid[]; };

// -------------------------------------------------------
// Constants
// -------------------------------------------------------
const int   TILE_SIZE = 16;
const float PI        = 3.14159265358979;

// -------------------------------------------------------
// Normal map decode -> world-space N
// -------------------------------------------------------
vec3 SampleNormal()
{
    vec3 n = texture(uNormalTex, vUV).rgb;
    n      = n * 2.0 - 1.0;
    return normalize(vTBN * n);
}

// -------------------------------------------------------
// GGX / Cook-Torrance BRDF — Filament / Epic reference impl
//
// Key differences from a naive implementation:
//
// D_GGX: numerically stable form from Filament spec.
//   Avoids fp cancellation at low roughness by reformulating
//   using Lagrange's identity instead of (1 - NdotH^2).
//
// V_SmithGGXCorrelated: height-correlated visibility term.
//   Replaces the split Schlick-GGX approximation. The
//   height-correlated form is more physically accurate as it
//   accounts for the correlation between masking and shadowing.
//   Crucially, this term already divides by (4·NdotV·NdotL),
//   so the specular output is just D·V·F — no extra division.
//
// Roughness remapping: perceptualRoughness -> alpha = r^2
//   Artists author roughness in perceptual (linear) space.
//   The BRDF operates on alpha = roughness^2 which gives a
//   more visually linear response across the 0..1 range.
//   Without this, low roughness values look far too rough.
//
// References:
//   Filament: https://google.github.io/filament/Filament.md.html
//   Epic/Karis: https://blog.selfshadow.com/publications/s2013-shading-course/
//   Walter et al.: Microfacet Models for Refraction (GGX origin paper)
// -------------------------------------------------------

// D — GGX Normal Distribution Function (Filament numerically stable form)
// Input: perceptual roughness already remapped to alpha = r^2 outside
float D_GGX(float NdotH, float alpha)
{
    float a  = NdotH * alpha;
    float k  = alpha / (1.0 - NdotH * NdotH + a * a);
    return k * k * (1.0 / PI);
}

// V — Smith GGX Height-Correlated Visibility Function (Filament)
// This term already incorporates the 1/(4·NdotV·NdotL) denominator
// from the Cook-Torrance specular BRDF, so specular = D·V·F directly.
float V_SmithGGXCorrelated(float NdotV, float NdotL, float alpha)
{
    float a2   = alpha * alpha;
    float GGXV = NdotL * sqrt((NdotV - NdotV * a2) * NdotV + a2);
    float GGXL = NdotV * sqrt((NdotL - NdotL * a2) * NdotL + a2);
    return 0.5 / max(GGXV + GGXL, 1e-5);
}

// F — Fresnel-Schlick
vec3 F_Schlick(float VdotH, vec3 F0)
{
    return F0 + (1.0 - F0) * pow(clamp(1.0 - VdotH, 0.0, 1.0), 5.0);
}

// -------------------------------------------------------
// Full Cook-Torrance BRDF
//   specular = D * V * F          (V already has 4·NdotV·NdotL baked in)
//   diffuse  = albedo / PI        (Lambertian)
//   kD       = (1 - F) * (1 - metallic)  — energy conservation
//   AO       applied to diffuse only, not specular
// -------------------------------------------------------
vec3 CookTorranceBRDF(vec3 N, vec3 V, vec3 L,
                      vec3 albedo, vec3 F0,
                      float alpha, float metallic, float ao)
{
    vec3  H     = normalize(V + L);
    float NdotV = max(abs(dot(N, V)), 1e-5); // abs handles backface normals gracefully
    float NdotL = clamp(dot(N, L), 0.0, 1.0);
    float NdotH = clamp(dot(N, H), 0.0, 1.0);
    float VdotH = clamp(dot(V, H), 0.0, 1.0);

    // Specular
    float D    = D_GGX(NdotH, alpha);
    float Vis  = V_SmithGGXCorrelated(NdotV, NdotL, alpha);
    vec3  F    = F_Schlick(VdotH, F0);
    vec3  spec = D * Vis * F;   // Vis already includes 1/(4·NdotV·NdotL)

    // Diffuse — metals have no diffuse
    // AO only on diffuse: direct specular is not self-occluded
    vec3 kD   = (vec3(1.0) - F) * (1.0 - metallic);
    vec3 diff = kD * albedo / PI * ao;

    return (diff + spec) * NdotL;
}

// -------------------------------------------------------
// ACES filmic tonemapping (Hill / Narkowicz approximation)
// Maps HDR [0,inf) to display [0,1] with pleasing highlight rolloff
// -------------------------------------------------------
vec3 ACESFilmic(vec3 x)
{
    const float a = 2.51;
    const float b = 0.03;
    const float c = 2.43;
    const float d = 0.59;
    const float e = 0.14;
    return clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0, 1.0);
}

// -------------------------------------------------------
// Forward+ tile light accumulation
//   Attenuation = intensity / d^2  (inverse-square law)
//   radius = culling boundary only, no effect on brightness
// -------------------------------------------------------
vec3 CalcPointLights(vec3 N, vec3 V,
                     vec3 albedo, vec3 F0,
                     float alpha, float metallic, float ao)
{
    ivec2 tile  = ivec2(gl_FragCoord.xy) / TILE_SIZE;
    int   tileW = (uScreenSize.x + TILE_SIZE - 1) / TILE_SIZE;
    uint  idx   = uint(tile.y * tileW + tile.x);

    vec3 result = vec3(0.0);

    for (uint i = grid[idx].offset; i < grid[idx].offset + grid[idx].count; i++)
    {
        PointLight l = lights[lightIndices[i]];

        vec3  lVec = l.posRadius.xyz - vWorldPos;
        float dist = length(lVec);
        float rad  = l.posRadius.w;
        if (dist > rad) continue;

        vec3  L    = lVec / dist;

        // Pure inverse-square — intensity in candelas, result in lux
        float atten = l.colorIntensity.a / max(dist * dist, 0.0001);
        vec3  Li    = l.colorIntensity.rgb * atten;

        result += CookTorranceBRDF(N, V, L, albedo, F0, alpha, metallic, ao) * Li;
    }

    return result;
}

// -------------------------------------------------------
// Main
// -------------------------------------------------------
void main()
{
    // Sample textures
    vec3  albedo            = texture(uAlbedoTex,    vUV).rgb;
    vec2  mr                = texture(uMRTex,        vUV).gb; // g=roughness, b=metallic
    float perceptualRoughness = clamp(mr.x, 0.045, 1.0); // 0.045 = Filament minimum
    float metallic          = clamp(mr.y, 0.0, 1.0);
    float ao                = texture(uOcclusionTex, vUV).r;
    ao                      = (ao < 0.001) ? 1.0 : ao; // unbound sampler fallback

    // Remap perceptual roughness to alpha for BRDF
    // This is the most important remapping: without it low roughness
    // values look too rough and the response feels non-linear to artists
    float alpha = perceptualRoughness * perceptualRoughness;

    vec3 N  = SampleNormal();
    vec3 V  = normalize(uCameraPos - vWorldPos);

    // F0: base reflectivity at normal incidence
    // Dielectrics: 0.04 (covers most non-metals)
    // Metals: F0 = albedo (tinted specular, no diffuse)
    vec3 F0 = mix(vec3(0.04), albedo, metallic);

    vec3 lit = CalcPointLights(N, V, albedo, F0, alpha, metallic, ao);

    // ACES tonemapping — required for physically scaled intensity/d^2 values
    vec3 color = ACESFilmic(lit);

    // Gamma correction: linear -> sRGB
    color = pow(color, vec3(1.0 / 2.2));

    // No ambient — unlit surfaces are pure black
    FragColor = vec4(color, 1.0);
}
