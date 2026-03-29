#version 430 core

// -------------------------------------------------------
// Varyings from vertex shader
// -------------------------------------------------------
in vec3 vWorldPos;
in vec2 vUV;
in vec3 vT;
in vec3 vB;
in vec3 vN;

// -------------------------------------------------------
// MRT output
// -------------------------------------------------------
layout(location = 0) out vec4 FragColor;

// -------------------------------------------------------
// Texture units
// -------------------------------------------------------
uniform sampler2D      uAlbedoTex;       // unit 0 — baseColor  (sRGB)
uniform sampler2D      uNormalTex;       // unit 1 — tangent-space normal
uniform sampler2D      uMRTex;           // unit 2 — G=roughness, B=metallic
uniform sampler2D      uOcclusionTex;    // unit 3 — R=AO
uniform sampler2D      uEmissiveTex;     // unit 4 — emissive
uniform samplerCubeArray uShadowCubeArray; // unit 5 — point light cubemap array
uniform sampler2DShadow  uDirShadowMap;    // unit 6 — directional light shadow map (hardware PCF)

// -------------------------------------------------------
// Per-frame uniforms
// -------------------------------------------------------
uniform vec3 uCameraPos;
uniform int  uShadowCount;
uniform int  uLightCount;
uniform int  uShadowRes;

// -------------------------------------------------------
// Point light SSBO  (binding 0)
// -------------------------------------------------------
struct PointLight {
    vec4 posRadius;       // xyz = world pos,  w = radius
    vec4 colorIntensity;  // rgb = color,       a = intensity
};
layout(std430, binding = 0) readonly buffer LightBuffer { PointLight lights[]; };

// -------------------------------------------------------
// Point light shadow SSBO  (binding 1)
// -------------------------------------------------------
struct ShadowData {
    mat4  lightSpaceMatrices[6];
    int   lightIndex;
    float farPlane;
    int   pad[2];
};
layout(std430, binding = 1) readonly buffer ShadowBuf { ShadowData shadows[]; };

// -------------------------------------------------------
// Directional light UBO  (binding 2)
//   colorEnabled.a == 0 → no directional light (skip term).
//   lightSpaceMatrix transforms world → shadow NDC for the
//   ortho shadow map.
// -------------------------------------------------------
layout(std140, binding = 2) uniform DirLightBlock {
    vec4 uDirLightDirIntensity;   // xyz = direction (world, toward scene), w = intensity
    vec4 uDirLightColorEnabled;   // rgb = color, a = 1.0 if light exists else 0.0
    mat4 uDirLightSpaceMatrix;    // ortho VP
};

// -------------------------------------------------------
// Constants
// -------------------------------------------------------
const float PI = 3.14159265358979;

// -------------------------------------------------------
// Normal map decode
// GL_TEXTURE_LOD_BIAS on the normal map texture object
// handles mip selection — no shader bias needed.
// -------------------------------------------------------
vec3 SampleNormal()
{
    vec3 n = texture(uNormalTex, vUV).rgb;
    n      = n * 2.0 - 1.0;

    vec3 T = normalize(vT);
    vec3 B = normalize(vB);
    vec3 N = normalize(vN);

    return normalize(mat3(T, B, N) * n);
}

// -------------------------------------------------------
// GGX / Cook-Torrance BRDF
// -------------------------------------------------------
float D_GGX(float NdotH, float alpha)
{
    float a = NdotH * alpha;
    float k = alpha / (1.0 - NdotH * NdotH + a * a);
    return k * k * (1.0 / PI);
}

float V_SmithGGXCorrelated(float NdotV, float NdotL, float alpha)
{
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

    float D   = D_GGX(NdotH, alpha);
    float Vis = V_SmithGGXCorrelated(NdotV, NdotL, alpha);
    vec3  F   = F_Schlick(VdotH, F0);
    vec3  spec = D * Vis * F;

    vec3 kD   = (vec3(1.0) - F) * (1.0 - metallic);
    vec3 diff = kD * albedo / PI;

    return (diff + spec) * NdotL;
}

// -------------------------------------------------------
// Poisson sphere — 32 samples for PCSS
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
// PCSS — point lights
// -------------------------------------------------------
float ShadowPCSS(int shadowIdx, vec3 fragToLight,
                 float currentDist, float farPlane,
                 float lightRadius, float NdotL)
{
    float normalizedDist = currentDist / farPlane;
    float bias           = max(0.005, 0.015 * (1.0 - NdotL));
    float refDepth       = normalizedDist - bias;
    float texelScale     = 1024.0 / float(uShadowRes);

    const int kBlockerSamples = 16;
    float searchRadius = (lightRadius / farPlane)
                       * (normalizedDist / 1.0)
                       * texelScale * 3.0;

    float blockerSum   = 0.0;
    int   blockerCount = 0;

    for (int i = 0; i < kBlockerSamples; i++)
    {
        vec3  sampleDir = normalize(fragToLight + kPoissonSphere[i] * searchRadius);
        float depth     = texture(uShadowCubeArray,
                                  vec4(sampleDir, float(shadowIdx))).r;
        if (depth < refDepth) { blockerSum += depth; blockerCount++; }
    }

    if (blockerCount == 0)               return 1.0;
    if (blockerCount == kBlockerSamples) return 0.0;

    float avgBlocker = blockerSum / float(blockerCount);
    float penumbra   = (normalizedDist - avgBlocker) / avgBlocker;
    penumbra = clamp(penumbra * lightRadius * texelScale * 0.12, 0.0, 0.25);

    const int kPCFSamples = 32;
    float angle  = fract(sin(dot(vWorldPos, vec3(12.9898, 78.233, 37.719))) * 43758.5453);
    float cosA   = cos(angle), sinA = sin(angle);

    float shadow = 0.0;
    for (int i = 0; i < kPCFSamples; i++)
    {
        vec3 offset  = kPoissonSphere[i];
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
// Directional light PCF shadow
// -------------------------------------------------------
const vec2 kPoissonDisk[16] = vec2[](
    vec2(-0.94201624, -0.39906216),
    vec2( 0.94558609, -0.76890725),
    vec2(-0.09418410, -0.92938870),
    vec2( 0.34495938,  0.29387760),
    vec2(-0.91588581,  0.45771432),
    vec2(-0.81544232, -0.87912464),
    vec2(-0.38277543,  0.27676845),
    vec2( 0.97484398,  0.75648379),
    vec2( 0.44323325, -0.97511554),
    vec2( 0.53742981, -0.47373420),
    vec2(-0.26496911, -0.41893023),
    vec2( 0.79197514,  0.19090188),
    vec2(-0.24188840,  0.99706507),
    vec2(-0.81409955,  0.91437590),
    vec2( 0.19984126,  0.78641367),
    vec2( 0.14383161, -0.14100790)
);

float DirShadowPCF(vec3 worldPos, float NdotL)
{
    vec4 lsPos      = uDirLightSpaceMatrix * vec4(worldPos, 1.0);
    vec3 projCoords = lsPos.xyz / lsPos.w;
    projCoords      = projCoords * 0.5 + 0.5;

    if (projCoords.x < 0.0 || projCoords.x > 1.0 ||
        projCoords.y < 0.0 || projCoords.y > 1.0 ||
        projCoords.z > 1.0)
        return 1.0;

    float bias         = max(0.005, 0.010 * (1.0 - NdotL));
    float compareDepth = projCoords.z - bias;

    const float kSpread  = 2.0;
    const float kTexelSz = 1.0 / float(uShadowRes);
    float shadow = 0.0;
    for (int i = 0; i < 16; i++)
    {
        vec2 offset = kPoissonDisk[i] * kTexelSz * kSpread;
        shadow += texture(uDirShadowMap,
                          vec3(projCoords.xy + offset, compareDepth));
    }
    return shadow / 16.0;
}

// -------------------------------------------------------
// Point light accumulation
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

        float atten = l.colorIntensity.a / max(dist * dist, 0.0001);
        float w     = max(0.0, 1.0 - (dist / rad) * (dist / rad));
        atten *= w * w;

        vec3  Li     = l.colorIntensity.rgb * atten;
        float shadow = GetShadowFactor(i, l.posRadius.xyz, dist, l.posRadius.w, NdotL);

        vec3 brdf = CookTorranceBRDF(N, V, L, albedo, F0, alpha, metallic);
        result += brdf * Li * shadow * ao;
    }

    return result;
}

// -------------------------------------------------------
// Directional light contribution
// -------------------------------------------------------
vec3 CalcDirLight(vec3 N, vec3 V,
                  vec3 albedo, vec3 F0,
                  float alpha, float metallic, float ao)
{
    if (uDirLightColorEnabled.a < 0.5)
        return vec3(0.0);

    vec3  L     = normalize(-uDirLightDirIntensity.xyz);
    float NdotL = clamp(dot(N, L), 0.0, 1.0);

    vec3 Li = uDirLightColorEnabled.rgb * uDirLightDirIntensity.w;

    float shadow = DirShadowPCF(vWorldPos, NdotL);

    vec3 brdf = CookTorranceBRDF(N, V, L, albedo, F0, alpha, metallic);
    return brdf * Li * shadow * ao;
}

// -------------------------------------------------------
// Main
// -------------------------------------------------------
void main()
{
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

    vec3 color = CalcPointLights(N, V, albedo, F0, alpha, metallic, ao)
               + CalcDirLight   (N, V, albedo, F0, alpha, metallic, ao);

    color += texture(uEmissiveTex, vUV).rgb;

    FragColor = vec4(color, 1.0);
}
