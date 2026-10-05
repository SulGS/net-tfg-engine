#version 430 core

// Pairs with fluid.vert. Reuses DefaultShader.hpp's SSBO/UBO layout, texture units and BRDF/shadow code verbatim (no
// #include in GLSL, so it's duplicated) so water lights/shadows like every other surface. Only the "material" section
// differs: no texture maps, a procedurally animated normal instead.

// -------------------------------------------------------
// Varyings from vertex shader
// -------------------------------------------------------
in vec3 vWorldPos;
in vec2 vUV;
in vec3 vT;
in vec3 vB;
in vec3 vN;

// -------------------------------------------------------
// MRT output (the GBuffer variant writes the engine's GBuffer instead, see GBufferVariant.hpp)
// -------------------------------------------------------
#ifndef GBUFFER_PASS
layout(location = 0) out vec4 FragColor;
#endif

// ---- Texture units ----
// Units 0-4 are the PBR maps (albedo, normal, metal/rough, occlusion, emissive); water has none, so it doesn't
// declare them. Mesh::draw() only binds those a shader has.
uniform samplerCubeArray uShadowCubeArray; // unit 5 — point light cubemap array
uniform samplerCubeArrayShadow uShadowCubeArrayCmp; // unit 8 - same cubemap array, depth-compare sampler (hardware PCF)
uniform sampler2DArrayShadow uDirShadowMap; // unit 6 - directional light cascades, depth-compare sampler (hardware PCF)
uniform sampler2DArray uDirShadowDepth;      // unit 9 - same cascades, raw depth (PCSS blocker search)
uniform sampler2D        uSSAOTex;         // unit 7 — screen-space AO, reduced res (all 1.0 when SSAO is off)
uniform int              uSSAOScale;       // its resolution divisor (1, 2 or 4)

// -------------------------------------------------------
// Per-frame uniforms
// -------------------------------------------------------
uniform vec3  uCameraPos;
uniform int   uShadowCount;
uniform int   uLightCount;
uniform int   uShadowRes;
uniform int   uDirShadowRes;

// ---- Water look (set once per Material, wherever the mesh is created) ----
uniform float uTime;
uniform vec3  uShallowColor;
uniform vec3  uDeepColor;
uniform vec3  uSkyColor;
uniform float uRoughness;

// -------------------------------------------------------
// Point light SSBO  (binding 0)
// -------------------------------------------------------
struct PointLight {
    vec4 posRadius;
    vec4 colorIntensity;
};
layout(std430, binding = 0) readonly buffer LightBuffer { PointLight lights[]; };

// -------------------------------------------------------
// Point light shadow SSBO  (binding 1)
// -------------------------------------------------------
struct ShadowData {
    mat4  lightSpaceMatrices[6];
    int   lightIndex;
    float farPlane;
    float sourceRadius;   // light size for PCSS
    int   pad;
};
layout(std430, binding = 1) readonly buffer ShadowBuf { ShadowData shadows[]; };

// -------------------------------------------------------
// Directional light UBO  (binding 2)
// -------------------------------------------------------
layout(std140, binding = 2) uniform DirLightBlock {
    vec4 uDirLightDirIntensity;
    vec4 uDirLightColorEnabled;
    mat4 uDirLightSpaceMatrices[4]; // per-cascade ortho VP (world -> NDC)
    vec4 uDirCascadeTexel;          // world size of one shadow texel, per cascade
    vec4 uDirCascadeDepthRange;     // world distance spanned by each cascade's [0,1] depth
    vec4 uDirLightParams;           // x = cascade count (0 = no shadow), y = tan(light angular radius)
};

const float PI = 3.14159265358979;

// -------------------------------------------------------
// GGX / Cook-Torrance BRDF — identical to the engine's default shader (DefaultShader.hpp)
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
// PCSS - point lights
// -------------------------------------------------------
// Interleaved gradient noise (Jimenez 2014): per-pixel rotation of the sample disks. Screen-stable, so it reads as a
// fine dither instead of the crawling grain of a world-position hash.
float InterleavedGradientNoise(vec2 p)
{
    return fract(52.9829189 * fract(dot(p, vec2(0.06711056, 0.00583715))));
}

// Vogel (golden-angle spiral) disk: even coverage of the unit disk for any sample count, rotated by phi.
vec2 VogelDisk(int i, int count, float phi)
{
    float r     = sqrt((float(i) + 0.5) / float(count));
    float theta = float(i) * 2.39996323 + phi;
    return r * vec2(cos(theta), sin(theta));
}

// Distance from the light to the receiver's tangent plane along dir: each tap compares against the surface it
// actually lands on, so wide penumbrae on sloped surfaces don't shadow themselves.
float ReceiverPlaneDist(vec3 dir, vec3 planeOffset, vec3 Ng, float fallback)
{
    float denom = dot(dir, Ng);
    if (denom > -0.05) return fallback; // grazing / facing away: the plane is no help there
    return clamp(dot(planeOffset, Ng) / denom, fallback * 0.5, fallback * 2.0);
}

// Everything is angular: offsets are added to the unit light->fragment direction, in the plane perpendicular to it,
// so an offset of k equals k / dist world units on the receiver. One cube texel is ~2/res of that.
float ShadowPCSS(int shadowIdx, vec3 lightPos, float farPlane, float sourceRadius)
{
    const int kBlockerSamples = 16;
    const int kPCFSamples     = 32;

    vec3  Ng       = normalize(vN); // geometric normal: the bias must not follow the normal map
    vec3  toFrag   = vWorldPos - lightPos;
    float dist     = length(toFrag);
    float NdotL    = clamp(dot(Ng, -toFrag / dist), 0.0, 1.0);
    float sinT     = sqrt(1.0 - NdotL * NdotL);

    float texelAngle = 2.0 / float(uShadowRes);
    float texelWorld = texelAngle * dist;

    // Normal offset (more at grazing angles) instead of a big depth bias: no acne, no detached "peter pan" shadows.
    vec3  samplePos   = vWorldPos + Ng * texelWorld * (0.5 + 1.5 * sinT);
    vec3  fragToLight = samplePos - lightPos;
    float receiverDist = length(fragToLight);
    vec3  dir          = fragToLight / receiverDist;
    float depthBias    = texelWorld * 0.5;

    vec3 up = (abs(dir.y) < 0.99) ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
    vec3 T  = normalize(cross(up, dir));
    vec3 B  = cross(dir, T);

    float phi   = InterleavedGradientNoise(gl_FragCoord.xy) * 6.28318531;
    float layer = float(shadowIdx);

    // 1) Blocker search over the part of the map the light's disk can be hidden by (covers blockers down to ~1/3 of
    //    the way to the light; anything closer would need an unbounded region).
    float searchRadius = clamp(2.0 * sourceRadius / receiverDist, texelAngle * 2.0, 0.3);

    float blockerSum   = 0.0;
    int   blockerCount = 0;
    for (int i = 0; i < kBlockerSamples; i++)
    {
        vec2  o     = VogelDisk(i, kBlockerSamples, phi) * searchRadius;
        vec3  d     = normalize(dir + T * o.x + B * o.y);
        float ref   = (ReceiverPlaneDist(d, fragToLight, Ng, receiverDist) - depthBias) / farPlane;
        float depth = texture(uShadowCubeArray, vec4(d, layer)).r;
        if (depth < ref) { blockerSum += depth; blockerCount++; }
    }

    if (blockerCount == 0) return 1.0;

    // 2) Penumbra width on the receiver from similar triangles, as an angle from the light. Never below ~1.5 texels so
    //    contact shadows stay antialiased, never past the region the search looked at.
    float blockerDist   = max(blockerSum / float(blockerCount) * farPlane, 1e-3);
    float penumbraWorld = sourceRadius * (receiverDist - blockerDist) / blockerDist;
    float filterRadius  = clamp(penumbraWorld / receiverDist, texelAngle * 1.5, searchRadius);

    // 3) PCF: each tap is a hardware bilinear comparison (unit 8), so even few texels give a smooth edge.
    float shadow = 0.0;
    for (int i = 0; i < kPCFSamples; i++)
    {
        vec2  o   = VogelDisk(i, kPCFSamples, phi) * filterRadius;
        vec3  d   = normalize(dir + T * o.x + B * o.y);
        float ref = (ReceiverPlaneDist(d, fragToLight, Ng, receiverDist) - depthBias) / farPlane;
        shadow += texture(uShadowCubeArrayCmp, vec4(d, layer), ref);
    }
    return shadow / float(kPCFSamples);
}

float GetShadowFactor(int lightBufIndex, vec3 lightPos)
{
    for (int s = 0; s < uShadowCount; s++)
    {
        if (shadows[s].lightIndex == lightBufIndex)
            return ShadowPCSS(s, lightPos, shadows[s].farPlane, shadows[s].sourceRadius);
    }
    return 1.0;
}

// -------------------------------------------------------
// Directional light - cascaded shadow maps with PCSS
// -------------------------------------------------------
// Penumbra = occluder-to-receiver distance x tan(light angular radius), in world units, so it matches across cascades.
// The blocker search assumes occluders at most this far from the receiver (beyond it the penumbra is underestimated).
const float kDirOccluderReach = 20.0;

// Shadow-map uv (xy) and depth (z) of a world position in cascade c.
vec3 DirShadowCoord(int c, vec3 p)
{
    vec4 ls = uDirLightSpaceMatrices[c] * vec4(p, 1.0);
    return ls.xyz / ls.w * 0.5 + 0.5;
}

float DirShadowCascade(int c, vec3 Ng, float NdotL, float phi)
{
    const int kBlockerSamples = 16;
    const int kPCFSamples     = 24;

    float texelWorld = uDirCascadeTexel[c];
    float depthRange = uDirCascadeDepthRange[c];
    float texelUV    = 1.0 / float(uDirShadowRes);
    float layer      = float(c);

    // Normal offset (more at grazing angles) instead of a big depth bias: no acne, no detached shadows.
    float sinT  = sqrt(1.0 - NdotL * NdotL);
    vec3  p     = vWorldPos + Ng * texelWorld * (0.5 + 1.5 * sinT);
    vec3  coord = DirShadowCoord(c, p);

    // Receiver plane: depth change per unit of uv across the surface, from two points on its tangent plane (analytic,
    // no screen derivatives). Each tap then compares against the surface it lands on, so wide penumbrae on sloped
    // surfaces don't shadow themselves. Capped at a slope of 4 so grazing surfaces don't swallow every shadow.
    vec3  t1  = normalize(cross(Ng, abs(Ng.z) < 0.99 ? vec3(0.0, 0.0, 1.0) : vec3(1.0, 0.0, 0.0)));
    vec3  t2  = cross(Ng, t1);
    vec3  a   = DirShadowCoord(c, p + t1) - coord;
    vec3  b   = DirShadowCoord(c, p + t2) - coord;
    float det = a.x * b.y - a.y * b.x;
    vec2  dz  = (abs(det) > 1e-12) ? vec2(a.z * b.y - a.y * b.z, a.x * b.z - a.z * b.x) / det : vec2(0.0);
    float maxGrad = 4.0 * texelWorld / texelUV / depthRange;
    if (length(dz) > maxGrad) dz *= maxGrad / length(dz);

    float bias = 0.5 * texelWorld / depthRange;

    // 1) Blocker search.
    float searchUV = clamp(uDirLightParams.y * kDirOccluderReach / texelWorld, 2.0, 32.0) * texelUV;

    float blockerSum   = 0.0;
    int   blockerCount = 0;
    for (int i = 0; i < kBlockerSamples; i++)
    {
        vec2  o     = VogelDisk(i, kBlockerSamples, phi) * searchUV;
        float ref   = coord.z + dot(dz, o) - bias;
        float depth = texture(uDirShadowDepth, vec3(coord.xy + o, layer)).r;
        if (depth < ref) { blockerSum += depth; blockerCount++; }
    }

    if (blockerCount == 0) return 1.0;

    // 2) Penumbra from the average blocker distance; at least 1.5 texels so hard contact edges stay antialiased.
    float blockerDist = max(coord.z - blockerSum / float(blockerCount), 0.0) * depthRange;
    float filterUV    = clamp(blockerDist * uDirLightParams.y / texelWorld * texelUV, 1.5 * texelUV, searchUV);

    // 3) PCF: each tap is a hardware bilinear comparison.
    float shadow = 0.0;
    for (int i = 0; i < kPCFSamples; i++)
    {
        vec2  o   = VogelDisk(i, kPCFSamples, phi) * filterUV;
        float ref = coord.z + dot(dz, o) - bias;
        shadow += texture(uDirShadowMap, vec4(coord.xy + o, layer, ref));
    }
    return shadow / float(kPCFSamples);
}

// Picks the first (finest) cascade whose map holds this fragment with room for the filter, and cross-fades into the
// next one near its border so the resolution change doesn't show as a seam. The last one fades out to unshadowed.
float DirShadow()
{
    int count = int(uDirLightParams.x);
    if (count == 0) return 1.0;

    vec3  Ng    = normalize(vN); // geometric normal: the bias must not follow the normal map
    vec3  L     = normalize(-uDirLightDirIntensity.xyz);
    float NdotL = clamp(dot(Ng, L), 0.0, 1.0);
    float phi   = InterleavedGradientNoise(gl_FragCoord.xy) * 6.28318531;

    const float kUsable     = 0.94; // past this (0 = map centre, 1 = edge) the filter would read outside the map
    const float kBlendStart = 0.80;

    for (int c = 0; c < count; c++)
    {
        vec3  coord = DirShadowCoord(c, vWorldPos);
        vec2  e     = abs(coord.xy - 0.5) * 2.0;
        float edge  = max(e.x, e.y);
        if (edge >= kUsable || coord.z > 1.0) continue;

        float shadow = DirShadowCascade(c, Ng, NdotL, phi);
        float w      = smoothstep(kBlendStart, kUsable, edge);
        if (w > 0.0)
            shadow = mix(shadow, (c + 1 < count) ? DirShadowCascade(c + 1, Ng, NdotL, phi) : 1.0, w);
        return shadow;
    }
    return 1.0;
}

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
        float shadow = GetShadowFactor(i, l.posRadius.xyz);

        vec3 brdf = CookTorranceBRDF(N, V, L, albedo, F0, alpha, metallic);
        result += brdf * Li * shadow * ao;
    }

    return result;
}

vec3 CalcDirLight(vec3 N, vec3 V,
                  vec3 albedo, vec3 F0,
                  float alpha, float metallic, float ao)
{
    if (uDirLightColorEnabled.a < 0.5)
        return vec3(0.0);

    vec3  L     = normalize(-uDirLightDirIntensity.xyz);
    float NdotL = clamp(dot(N, L), 0.0, 1.0);

    vec3 Li = uDirLightColorEnabled.rgb * uDirLightDirIntensity.w;

    float shadow = DirShadow();

    vec3 brdf = CookTorranceBRDF(N, V, L, albedo, F0, alpha, metallic);
    return brdf * Li * shadow * ao;
}

// -------------------------------------------------------
// Procedural surface — this is the only part that differs from the default shader
// -------------------------------------------------------

float hash21(vec2 p)
{
    p = fract(p * vec2(123.34, 456.21));
    p += dot(p, p + 45.32);
    return fract(p.x * p.y);
}

float valueNoise(vec2 p)
{
    vec2 i = floor(p);
    vec2 f = fract(p);
    float a = hash21(i);
    float b = hash21(i + vec2(1.0, 0.0));
    float c = hash21(i + vec2(0.0, 1.0));
    float d = hash21(i + vec2(1.0, 1.0));
    vec2  u = f * f * (3.0 - 2.0 * f);
    return mix(mix(a, b, u.x), mix(c, d, u.x), u.y);
}

// Two scrolling noise layers at different scale/speed/direction (classic
// "flow" trick — a single layer tiles/loops obviously, two never repeat in
// sync) turned into a tangent-space bump normal via the analytic gradient.
vec3 SampleRippleNormal()
{
    vec2 uv1 = vWorldPos.xy * 0.15 + vec2(uTime * 0.06,  uTime * 0.035);
    vec2 uv2 = vWorldPos.xy * 0.37 + vec2(-uTime * 0.025, uTime * 0.05);

    const float e = 0.06;
    float h1  = valueNoise(uv1);
    float h1x = valueNoise(uv1 + vec2(e, 0.0));
    float h1y = valueNoise(uv1 + vec2(0.0, e));
    float h2  = valueNoise(uv2);
    float h2x = valueNoise(uv2 + vec2(e, 0.0));
    float h2y = valueNoise(uv2 + vec2(0.0, e));

    vec2 grad = ((vec2(h1x, h1y) - h1) + (vec2(h2x, h2y) - h2) * 0.6) / e;

    vec3 n = normalize(vec3(-grad * 0.55, 1.0));

    vec3 T = normalize(vT);
    vec3 B = normalize(vB);
    vec3 N = normalize(vN);
    return normalize(mat3(T, B, N) * n);
}

void main()
{
    vec3 N      = SampleRippleNormal();
    vec3 albedo = mix(uDeepColor, uShallowColor, 0.35);

#ifdef GBUFFER_PASS
    // Waves (fluid.vert) and ripples reach SSAO/SSR. hasEnvReflection = true: SSR replaces the sky fallback below
    // where its rays hit instead of adding on top of it.
    WriteGBuffer(N, uRoughness, 0.0, albedo, true);
#else
    vec3 V = normalize(uCameraPos - vWorldPos);

    float NdotV    = clamp(dot(N, V), 0.0, 1.0);
    float fresnel  = mix(0.02, 1.0, pow(1.0 - NdotV, 5.0));

    // Dielectric F0 (~0.02 for water); no metallic term.
    vec3  F0     = vec3(0.02);
    float alpha  = max(uRoughness * uRoughness, 0.001);

    // Reduced-res SSAO: texel p holds full pixel s*p, bilinear upscale aligned to that.
    float ao = texture(uSSAOTex, ((gl_FragCoord.xy - 0.5) / float(max(uSSAOScale, 1)) + 0.5)
                                 / vec2(textureSize(uSSAOTex, 0))).r;

    vec3 lit = CalcPointLights(N, V, albedo, F0, alpha, 0.0, ao)
             + CalcDirLight   (N, V, albedo, F0, alpha, 0.0, ao);

    // Fallback environment reflection (no cubemap capture): the sky colour,
    // blended in harder at grazing angles the way real Fresnel behaves. Where
    // an SSR ray hits, the SSR pass swaps this share for the traced reflection.
    vec3 reflected = uSkyColor;

    // Sharp specular glint from the sun — makes the water read as wet/shiny
    // even where the diffuse BRDF term alone would look flat.
    vec3 color = mix(lit, reflected, fresnel);

    if (uDirLightColorEnabled.a > 0.5)
    {
        vec3  L = normalize(-uDirLightDirIntensity.xyz);
        vec3  H = normalize(V + L);
        float glint = pow(clamp(dot(N, H), 0.0, 1.0), 512.0);
        color += glint * uDirLightColorEnabled.rgb * uDirLightDirIntensity.w * 0.6;
    }

    FragColor = vec4(color, 1.0);
#endif
}
