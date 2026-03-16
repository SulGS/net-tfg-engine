#version 430 core

// -------------------------------------------------------
// Varyings from vertex shader
// -------------------------------------------------------
in vec3 vWorldPos;
in vec2 vUV;
in mat3 vTBN;

// -------------------------------------------------------
// MRT outputs
//   layout 0 — HDR radiance
// -------------------------------------------------------
layout(location = 0) out vec4 FragColor;

// -------------------------------------------------------
// Texture units  (bound per-submesh by Mesh::render)
// -------------------------------------------------------
uniform sampler2D uAlbedoTex;    // unit 0 — baseColor  (sRGB)
uniform sampler2D uNormalTex;    // unit 1 — tangent-space normal
uniform sampler2D uMRTex;        // unit 2 — G=roughness, B=metallic
uniform sampler2D uOcclusionTex; // unit 3 — R=AO
uniform sampler2D uEmissiveTex;  // unit 4 — emissive (unused)

// unit 5 — cube shadow map array.
// samplerCubeArray (NOT Shadow) because the shadow pass writes
// LINEAR depth (dist/farPlane). We compare manually in ShadowPCF
// so that the reference and stored values are in the same space.
uniform samplerCubeArray uShadowCubeArray;

// -------------------------------------------------------
// Per-frame uniforms
// -------------------------------------------------------
uniform vec3 uCameraPos;
uniform int  uShadowCount;
uniform int  uLightCount;
uniform int  uShadowRes;

// -------------------------------------------------------
// Light SSBO  (binding 0)
// -------------------------------------------------------
struct PointLight {
    vec4 posRadius;       // xyz = world pos,  w = radius
    vec4 colorIntensity;  // rgb = color,       a = intensity (candelas)
};

layout(std430, binding = 0) readonly buffer LightBuffer { PointLight lights[]; };

// -------------------------------------------------------
// Shadow SSBO  (binding 1)
// -------------------------------------------------------
struct ShadowData {
    mat4  lightSpaceMatrices[6];
    int   lightIndex;
    float farPlane;
    int   pad[2];
};

layout(std430, binding = 1) readonly buffer ShadowBuf { ShadowData shadows[]; };

// -------------------------------------------------------
// Constants
// -------------------------------------------------------
const float PI = 3.14159265358979;

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
// -------------------------------------------------------

float D_GGX(float NdotH, float alpha)
{
    float a  = NdotH * alpha;
    float k  = alpha / (1.0 - NdotH * NdotH + a * a);
    return k * k * (1.0 / PI);
}

float V_SmithGGXCorrelated(float NdotV, float NdotL, float alpha)
{
    // Heitz 2014 height-correlated Smith G2 visibility function.
    // Ref: Filament §4.4.2, Lagarde "Moving Frostbite to PBR".
    //
    // Correct form:  GGXV = NdotL * sqrt(NdotV²*(1-α²) + α²)
    // Previous code: (NdotV - NdotV*a2)*NdotV + a2
    //              = NdotV² - NdotV²*a2 + a2  ← correct by coincidence,
    //                but the factoring is misleading and fragile under
    //                rearrangement. Rewrite explicitly for clarity.
    float a2   = alpha * alpha;
    float GGXV = NdotL * sqrt(NdotV * NdotV * (1.0 - a2) + a2);
    float GGXL = NdotV * sqrt(NdotL * NdotL * (1.0 - a2) + a2);
    return 0.5 / max(GGXV + GGXL, 1e-5);
}

vec3 F_Schlick(float VdotH, vec3 F0)
{
    return F0 + (1.0 - F0) * pow(clamp(1.0 - VdotH, 0.0, 1.0), 5.0);
}

vec3 CookTorranceBRDF(vec3 N, vec3 V, vec3 L,
                      vec3 albedo, vec3 F0,
                      float alpha, float metallic)
{
    vec3  H     = normalize(V + L);
    float NdotV = max(abs(dot(N, V)), 1e-5);
    float NdotL = clamp(dot(N, L), 0.0, 1.0);
    float NdotH = clamp(dot(N, H), 0.0, 1.0);
    float VdotH = clamp(dot(V, H), 0.0, 1.0);

    float D    = D_GGX(NdotH, alpha);
    float Vis  = V_SmithGGXCorrelated(NdotV, NdotL, alpha);
    vec3  F    = F_Schlick(VdotH, F0);
    vec3  spec = D * Vis * F;

    // AO removed — applied to the full (diff+spec) result in CalcPointLights
    // so both lobes are occluded equally. Baking it only into diff caused
    // over-bright specular on surfaces inside crevices/shadowed areas.
    vec3 kD   = (vec3(1.0) - F) * (1.0 - metallic);
    vec3 diff = kD * albedo / PI;

    return (diff + spec) * NdotL;
}

// -------------------------------------------------------
// Poisson disk on a sphere — 32 stratified samples for
// PCSS blocker search and the final PCF filter.
// Precomputed; zero per-frame cost.
// -------------------------------------------------------
const vec3 kPoissonSphere[32] = vec3[](
    vec3( 0.286,  0.928,  0.238), vec3(-0.612,  0.529, -0.588),
    vec3( 0.751, -0.432,  0.499), vec3(-0.178, -0.861,  0.477),
    vec3(-0.823,  0.124,  0.555), vec3( 0.438,  0.322, -0.841),
    vec3(-0.349, -0.213, -0.912), vec3( 0.918, -0.213, -0.335),
    vec3( 0.110,  0.623, -0.774), vec3(-0.752, -0.601,  0.271),
    vec3( 0.571,  0.763,  0.303), vec3( 0.042, -0.487,  0.872),
    vec3(-0.947,  0.219, -0.232), vec3( 0.687, -0.719,  0.103),
    vec3(-0.412,  0.786, -0.461), vec3( 0.247, -0.918, -0.308),
    vec3(-0.134,  0.373,  0.918), vec3( 0.826,  0.528, -0.197),
    vec3(-0.573, -0.448,  0.686), vec3( 0.358,  0.112,  0.927),
    vec3(-0.209, -0.761, -0.614), vec3( 0.614,  0.158, -0.773),
    vec3(-0.772, -0.238, -0.589), vec3( 0.188,  0.961, -0.203),
    vec3( 0.523, -0.348, -0.778), vec3(-0.466,  0.671,  0.577),
    vec3( 0.841, -0.421,  0.341), vec3(-0.313, -0.534,  0.786),
    vec3( 0.138, -0.289, -0.948), vec3(-0.689,  0.348, -0.636),
    vec3( 0.451,  0.883, -0.132), vec3(-0.871, -0.412,  0.268)
);

// -------------------------------------------------------
// PCSS — Percentage Closer Soft Shadows for point lights.
//
// Shadow pass writes linear depth:
//   gl_FragDepth = length(fragPos - lightPos) / farPlane
// All comparisons are in that same [0,1] linear space.
//
// Algorithm:
//   1. Blocker search  — average depth of occluders in a
//      search radius proportional to the light source size.
//   2. Penumbra size   — (receiverDist - avgBlocker) / avgBlocker
//                        scaled by the light's physical radius.
//   3. PCF filter      — jittered Poisson disk whose radius
//                        equals the computed penumbra.
//
// Contact hardening falls out naturally: surfaces that
// touch the caster get a near-zero penumbra (hard shadow);
// surfaces far from the caster get a wide penumbra (soft).
//
// Returns 1.0 = fully lit, 0.0 = fully in shadow.
// -------------------------------------------------------
float ShadowPCSS(int shadowIdx, vec3 fragToLight,
                 float currentDist, float farPlane,
                 float lightRadius, float NdotL)
{
    float normalizedDist = currentDist / farPlane;

    // Slope-scaled bias: larger on grazing surfaces (low NdotL) where
    // shadow acne is worst, smaller on surfaces facing the light directly.
    // Previous code did dot(normalize(x), normalize(x)) which is always
    // 1.0 — the bias was a constant 0.005 with no slope scaling at all.
    float bias     = max(0.005, 0.015 * (1.0 - NdotL));
    float refDepth = normalizedDist - bias;

    // Resolution scale: coarser maps need larger kernels to
    // avoid visible texel boundaries at the same scene scale.
    float texelScale = 1024.0 / float(uShadowRes);

    // --------------------------------------------------
    // Pass 1 — Blocker search
    //   Sample a disk proportional to lightRadius / dist.
    //   Only pixels closer than the receiver count as blockers.
    // --------------------------------------------------
    const int kBlockerSamples = 16; // first half of kPoissonSphere
    float searchRadius = (lightRadius / farPlane)
                       * (normalizedDist / 1.0)  // grows with distance
                       * texelScale * 3.0;

    float blockerSum   = 0.0;
    int   blockerCount = 0;

    for (int i = 0; i < kBlockerSamples; i++)
    {
        vec3  sampleDir = normalize(fragToLight + kPoissonSphere[i] * searchRadius);
        float depth     = texture(uShadowCubeArray,
                                  vec4(sampleDir, float(shadowIdx))).r;
        if (depth < refDepth)
        {
            blockerSum += depth;
            blockerCount++;
        }
    }

    // Fully lit — no occluders found in search radius
    if (blockerCount == 0) return 1.0;

    // Fully in shadow — every sample was an occluder
    // (avoids division by zero; rare edge case near caster interior)
    if (blockerCount == kBlockerSamples) return 0.0;

    float avgBlocker = blockerSum / float(blockerCount);

    // --------------------------------------------------
    // Pass 2 — Penumbra estimation
    //   Physically derived from similar triangles:
    //     penumbraWidth ∝ lightRadius × (d_receiver − d_blocker) / d_blocker
    //   Clamped so very distant lights don't blur infinitely.
    // --------------------------------------------------
    float penumbra = (normalizedDist - avgBlocker) / avgBlocker;
    penumbra = clamp(penumbra * lightRadius * texelScale * 0.12, 0.0, 0.25);

    // --------------------------------------------------
    // Pass 3 — PCF with Poisson jitter
    //   Uses all 32 samples for a smooth, stratified filter.
    // --------------------------------------------------
    const int kPCFSamples = 32;

    // Rotate the disk per-pixel to break up banding.
    // Hash based on world position — stable across frames.
    float angle  = fract(sin(dot(vWorldPos, vec3(12.9898, 78.233, 37.719))) * 43758.5453);
    float cosA   = cos(angle), sinA = sin(angle);

    float shadow = 0.0;
    for (int i = 0; i < kPCFSamples; i++)
    {
        // Rotate sample around fragToLight axis for consistent 3-D jitter
        vec3 offset = kPoissonSphere[i];
        vec3 rotated = vec3(
            cosA * offset.x - sinA * offset.y,
            sinA * offset.x + cosA * offset.y,
            offset.z
        ) * penumbra;

        vec3  sampleDir   = fragToLight + rotated;
        float storedDepth = texture(uShadowCubeArray,
                                    vec4(sampleDir, float(shadowIdx))).r;
        shadow += (storedDepth > refDepth) ? 1.0 : 0.0;
    }
    return shadow / float(kPCFSamples);
}

// -------------------------------------------------------
// Returns PCSS shadow factor for a given light, or 1.0 if
// the light has no shadow map.
// lightRadius is the physical light source radius (posRadius.w)
// and drives the penumbra size in the PCSS blocker search.
// -------------------------------------------------------
float GetShadowFactor(int lightBufIndex, vec3 lightPos,
                      float currentDist, float lightRadius, float NdotL)
{
    for (int s = 0; s < uShadowCount; s++)
    {
        if (shadows[s].lightIndex == lightBufIndex)
        {
            vec3 fragToLight = vWorldPos - lightPos;
            return ShadowPCSS(s, fragToLight, currentDist,
                              shadows[s].farPlane, lightRadius, NdotL);
        }
    }
    return 1.0;
}

// -------------------------------------------------------
// Point light accumulation — iterate all visible lights
// -------------------------------------------------------
vec3 CalcPointLights(vec3 N, vec3 V,
                     vec3 albedo, vec3 F0,
                     float alpha, float metallic, float ao)
{
    vec3 result = vec3(0.0);

    for (int i = 0; i < uLightCount; i++)
    {
        PointLight l = lights[i];

        vec3  lVec = l.posRadius.xyz - vWorldPos;
        float dist = length(lVec);
        float rad  = l.posRadius.w;
        if (dist > rad) continue;

        vec3  L     = lVec / dist;
        float NdotL = clamp(dot(N, L), 0.0, 1.0);

        // Inverse-square physically correct falloff.
        float atten = l.colorIntensity.a / max(dist * dist, 0.0001);

        // UE4 / Karis smooth windowing: saturate(1 - (dist/rad)^2)^2
        // Brings attenuation smoothly to zero at exactly dist == rad,
        // eliminating the hard-edge luminance pop from a plain cutoff.
        float w = max(0.0, 1.0 - (dist / rad) * (dist / rad));
        atten  *= w * w;

        vec3 Li = l.colorIntensity.rgb * atten;

        // NdotL passed to shadow so bias can be slope-scaled properly.
        float shadow = GetShadowFactor(i, l.posRadius.xyz, dist, l.posRadius.w, NdotL);

        // AO applied to the full contribution — both diffuse and specular
        // are occluded equally, matching the intent of the occlusion map.
        vec3 brdf = CookTorranceBRDF(N, V, L, albedo, F0, alpha, metallic);
        result += brdf * Li * shadow * ao;
    }

    return result;
}

// -------------------------------------------------------
// Main
// -------------------------------------------------------
void main()
{
    // Albedo is already linear — loader uploads as GL_SRGB8_ALPHA8 /
    // GL_COMPRESSED_SRGB_ALPHA_BPTC_UNORM so the hardware decodes on sample.
    vec3  albedo              = texture(uAlbedoTex,    vUV).rgb;
    vec2  mr                  = texture(uMRTex,        vUV).gb;
    float perceptualRoughness = clamp(mr.x, 0.045, 1.0);
    float metallic            = clamp(mr.y, 0.0,   1.0);
    float ao                  = texture(uOcclusionTex, vUV).r;
          ao                  = (ao < 0.001) ? 1.0 : ao;

    float alpha = perceptualRoughness * perceptualRoughness;

    vec3 N  = SampleNormal();
    vec3 V  = normalize(uCameraPos - vWorldPos);
    vec3 F0 = mix(vec3(0.04), albedo, metallic);

    // Accumulate HDR lighting — values may freely exceed 1.0
    vec3 color = CalcPointLights(N, V, albedo, F0, alpha, metallic, ao);

    // Add emissive contribution — linear, HDR-friendly, loaded without sRGB decode
    color += texture(uEmissiveTex, vUV).rgb;

    // Write raw HDR radiance — TonemapPass resolves this to LDR
    // (no ACESFilmic, no gamma here)
    FragColor = vec4(color, 1.0);
}