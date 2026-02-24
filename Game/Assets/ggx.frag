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

// unit 5 — cube shadow map array.
// samplerCubeArray (NOT Shadow) because the shadow pass writes
// LINEAR depth (dist/farPlane). We compare manually in ShadowPCF
// so that the reference and stored values are in the same space.
uniform samplerCubeArray uShadowCubeArray;

// -------------------------------------------------------
// Per-frame uniforms
// -------------------------------------------------------
uniform ivec2 uScreenSize;
uniform vec3  uCameraPos;
uniform int   uShadowCount;

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
// Shadow SSBO  (binding 3)
// -------------------------------------------------------
struct ShadowData {
    mat4  lightSpaceMatrices[6];
    int   lightIndex;
    float farPlane;
    int   pad[2];
};

layout(std430, binding = 3) readonly buffer ShadowBuf { ShadowData shadows[]; };

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
// -------------------------------------------------------

float D_GGX(float NdotH, float alpha)
{
    float a  = NdotH * alpha;
    float k  = alpha / (1.0 - NdotH * NdotH + a * a);
    return k * k * (1.0 / PI);
}

float V_SmithGGXCorrelated(float NdotV, float NdotL, float alpha)
{
    float a2   = alpha * alpha;
    float GGXV = NdotL * sqrt((NdotV - NdotV * a2) * NdotV + a2);
    float GGXL = NdotV * sqrt((NdotL - NdotL * a2) * NdotL + a2);
    return 0.5 / max(GGXV + GGXL, 1e-5);
}

vec3 F_Schlick(float VdotH, vec3 F0)
{
    return F0 + (1.0 - F0) * pow(clamp(1.0 - VdotH, 0.0, 1.0), 5.0);
}

vec3 CookTorranceBRDF(vec3 N, vec3 V, vec3 L,
                      vec3 albedo, vec3 F0,
                      float alpha, float metallic, float ao)
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

    vec3 kD   = (vec3(1.0) - F) * (1.0 - metallic);
    vec3 diff = kD * albedo / PI * ao;

    return (diff + spec) * NdotL;
}

// -------------------------------------------------------
// ACES filmic tonemapping
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
// PCF soft shadows — manual comparison against LINEAR depth.
//
// The shadow pass fragment shader writes:
//   gl_FragDepth = length(fragPos - lightPos) / farPlane
//
// So we compare: currentDist/farPlane vs stored depth.
// Both are in [0,1] linear space — bias is stable everywhere.
//
// Returns 1.0 = fully lit, 0.0 = fully in shadow.
// -------------------------------------------------------
float ShadowPCF(int shadowIdx, vec3 fragToLight, float currentDist, float farPlane)
{
    const vec3 offsets[20] = vec3[](
        vec3( 1, 1, 1), vec3( 1,-1, 1), vec3(-1,-1, 1), vec3(-1, 1, 1),
        vec3( 1, 1,-1), vec3( 1,-1,-1), vec3(-1,-1,-1), vec3(-1, 1,-1),
        vec3( 1, 1, 0), vec3( 1,-1, 0), vec3(-1,-1, 0), vec3(-1, 1, 0),
        vec3( 1, 0, 1), vec3(-1, 0, 1), vec3( 1, 0,-1), vec3(-1, 0,-1),
        vec3( 0, 1, 1), vec3( 0,-1, 1), vec3( 0,-1,-1), vec3( 0, 1,-1)
    );

    float normalizedDist = currentDist / farPlane;

    // Fixed bias in linear depth space — stable at all distances
    float bias       = 0.01;
    float refDepth   = normalizedDist - bias;

    // Disk radius scales slightly with distance for softer far penumbra
    float diskRadius = (1.0 + normalizedDist) * 0.015;

    float shadow = 0.0;
    for (int i = 0; i < 20; i++)
    {
        vec3  sampleDir   = fragToLight + offsets[i] * diskRadius;
        // Read linear depth stored by shadow pass
        float storedDepth = texture(uShadowCubeArray,
                                    vec4(sampleDir, float(shadowIdx))).r;
        // Manual comparison: lit if stored depth > reference
        shadow += (storedDepth > refDepth) ? 1.0 : 0.0;
    }
    return shadow / 20.0;
}

// -------------------------------------------------------
// Returns PCF shadow factor for a given light, or 1.0 if
// the light has no shadow map.
// -------------------------------------------------------
float GetShadowFactor(uint lightBufIndex, vec3 lightPos, float currentDist)
{
    for (int s = 0; s < uShadowCount; s++)
    {
        if (shadows[s].lightIndex == int(lightBufIndex))
        {
            vec3 fragToLight = vWorldPos - lightPos;
            return ShadowPCF(s, fragToLight, currentDist, shadows[s].farPlane);
        }
    }
    return 1.0;
}

// -------------------------------------------------------
// Forward+ tile light accumulation
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
        uint       li = lightIndices[i];
        PointLight l  = lights[li];

        vec3  lVec = l.posRadius.xyz - vWorldPos;
        float dist = length(lVec);
        float rad  = l.posRadius.w;
        if (dist > rad) continue;

        vec3  L     = lVec / dist;
        float atten = l.colorIntensity.a / max(dist * dist, 0.0001);
        vec3  Li    = l.colorIntensity.rgb * atten;

        float shadow = GetShadowFactor(li, l.posRadius.xyz, dist);

        result += CookTorranceBRDF(N, V, L, albedo, F0, alpha, metallic, ao) * Li * shadow;
    }

    return result;
}

// -------------------------------------------------------
// Main
// -------------------------------------------------------
void main()
{
    vec3  albedo              = texture(uAlbedoTex,    vUV).rgb;
    vec2  mr                  = texture(uMRTex,        vUV).gb;
    float perceptualRoughness = clamp(mr.x, 0.045, 1.0);
    float metallic            = clamp(mr.y, 0.0, 1.0);
    float ao                  = texture(uOcclusionTex, vUV).r;
    ao                        = (ao < 0.001) ? 1.0 : ao;

    float alpha = perceptualRoughness * perceptualRoughness;

    vec3 N  = SampleNormal();
    vec3 V  = normalize(uCameraPos - vWorldPos);
    vec3 F0 = mix(vec3(0.04), albedo, metallic);

    vec3 lit   = CalcPointLights(N, V, albedo, F0, alpha, metallic, ao);
    vec3 color = ACESFilmic(lit);
    color      = pow(color, vec3(1.0 / 2.2));

    FragColor = vec4(color, 1.0);
}
