#pragma once
// FORWARD+ LIGHT GRID (tiled light culling). Before ShadingPass a compute shader (CullSource) splits the render target
// into kTileSize x kTileSize pixel tiles and writes, per tile, the list of point lights whose sphere reaches what that
// tile sees: the tile's frustum (4 side planes) cut to the depth range of the GBuffer inside it, when the GBuffer ran
// this frame (whole near..far otherwise). Lit shaders then visit only their tile's list instead of every light:
//
//     #include "engine/light_grid.glsl"
//     ...
//     int base;
//     int n = LightGridBegin(uLightCount, base);
//     for (int k = 0; k < n; k++) {
//         int i = LightGridLight(base, k);   // index into lights[] (and GetShadowFactor), as before
//         ...
//     }
//
// With uLightGridEnabled == 0 (probe captures: another viewpoint and resolution; no compute support; the
// NETTFG_NO_FORWARD_PLUS=1 switch) the same loop visits all uLightCount lights, the old behaviour.
//
// Buffer layout (SSBO binding kBinding): kWordsPerTile uints per tile, row-major tiles. Word 0 = light count, words
// 1.. = light indices, two 16-bit indices per word (low half first), so up to kMaxLightsPerTile per tile; beyond that
// the extra lights are dropped for that tile. Works with MSAA: the shading pass only indexes by gl_FragCoord.

namespace LightGrid
{
    inline constexpr const char* IncludeKey = "engine/light_grid.glsl";

    // Keep in sync with the literals in Source / CullSource (GLSL gets them as plain numbers).
    inline constexpr int kTileSize = 16;
    inline constexpr int kWordsPerTile = 128;
    inline constexpr int kMaxLightsPerTile = (kWordsPerTile - 1) * 2;   // 254
    inline constexpr int kBinding = 4;   // SSBO (0 = lights, 1 = shadows, 3 = env probes)

    inline constexpr const char* Source = R"GLSL(
// ---- Forward+ light grid (engine/light_grid.glsl, see LightGrid.hpp) ----
layout(std430, binding = 4) readonly buffer LightGridBuffer { uint lightGrid[]; };
uniform int uLightGridEnabled;   // 0: visit every light (probe captures, Forward+ off)
uniform int uLightGridTilesX;    // tiles per row

// Number of lights to visit for this fragment; `base` is then passed to LightGridLight.
int LightGridBegin(int lightCount, out int base)
{
    if (uLightGridEnabled == 0) { base = -1; return lightCount; }
    ivec2 tile = ivec2(gl_FragCoord.xy) / 16;
    base = (tile.y * uLightGridTilesX + tile.x) * 128;
    return int(lightGrid[base]);
}

// k-th light (0-based) of the list LightGridBegin opened: an index into lights[].
int LightGridLight(int base, int k)
{
    if (base < 0) return k;
    uint w = lightGrid[base + 1 + (k >> 1)];
    return int(((k & 1) == 0) ? (w & 0xFFFFu) : (w >> 16));
}
)GLSL";

    // One 16x16 work group per tile, one thread per pixel for the depth bounds, then the lights split across threads.
    inline constexpr const char* CullSource = R"GLSL(
#version 430 core
layout(local_size_x = 16, local_size_y = 16) in;

struct PointLight {
    vec4 posRadius;       // xyz = world pos,  w = radius
    vec4 colorIntensity;  // rgb = color,       a = intensity
};
layout(std430, binding = 0) readonly buffer LightBuffer { PointLight lights[]; };
layout(std430, binding = 4) writeonly buffer LightGridBuffer { uint lightGrid[]; };

uniform sampler2D uDepthTex;      // GBuffer depth (window depth, 1.0 = nothing drawn)
uniform int   uUseDepth;          // 0: no GBuffer this frame, tiles span near..far
uniform int   uLightCount;
uniform ivec2 uScreenSize;
uniform int   uTilesX;
uniform mat4  uView;
uniform mat4  uInvProjection;

const uint kMaxLights = 254u;

shared uint  sMinDepth;
shared uint  sMaxDepth;
shared uint  sCount;
shared uint  sList[254];
shared vec4  sPlanes[4];
shared float sZNear;              // view-space z bounds of what the tile sees (z < 0 in front of the camera)
shared float sZFar;
shared bool  sEmpty;

vec3 ViewPos(vec2 ndcXY, float ndcZ)
{
    vec4 v = uInvProjection * vec4(ndcXY, ndcZ, 1.0);
    return v.xyz / v.w;
}

void main()
{
    uvec2 tile = gl_WorkGroupID.xy;
    uint lid = gl_LocalInvocationIndex;

    if (lid == 0u) {
        sMinDepth = floatBitsToUint(1.0);
        sMaxDepth = 0u;
        sCount = 0u;
    }
    barrier();

    // Depth bounds. Depths are in [0, 1], where the float bit patterns order like the values.
    if (uUseDepth != 0) {
        ivec2 px = ivec2(gl_GlobalInvocationID.xy);
        if (px.x < uScreenSize.x && px.y < uScreenSize.y) {
            float d = texelFetch(uDepthTex, px, 0).r;
            if (d < 1.0) {
                atomicMin(sMinDepth, floatBitsToUint(d));
                atomicMax(sMaxDepth, floatBitsToUint(d));
            }
        }
    }
    barrier();

    if (lid == 0u) {
        float dMin = 0.0, dMax = 1.0;
        if (uUseDepth != 0) {
            dMin = uintBitsToFloat(sMinDepth);
            dMax = uintBitsToFloat(sMaxDepth);
        }
        sEmpty = dMin > dMax;   // only sky in this tile: no lit surface

        // Tile rectangle in NDC (the last row/column may be partial).
        vec2 p0 = vec2(tile * 16u) / vec2(uScreenSize) * 2.0 - 1.0;
        vec2 p1 = vec2(min((tile + 1u) * 16u, uvec2(uScreenSize))) / vec2(uScreenSize) * 2.0 - 1.0;
        vec2 c[4] = vec2[4](vec2(p0.x, p0.y), vec2(p1.x, p0.y), vec2(p1.x, p1.y), vec2(p0.x, p1.y));

        // Side planes through the near and far corners of the full tile frustum: never degenerate, and right for
        // perspective and orthographic projections alike. Oriented so the tile centre is on the positive side.
        vec3 inside = ViewPos((p0 + p1) * 0.5, 0.0);
        for (int e = 0; e < 4; e++) {
            vec3 nA = ViewPos(c[e], -1.0);
            vec3 fA = ViewPos(c[e], 1.0);
            vec3 fB = ViewPos(c[(e + 1) & 3], 1.0);
            vec3 n = normalize(cross(fA - nA, fB - nA));
            float d = -dot(n, nA);
            if (dot(n, inside) + d < 0.0) { n = -n; d = -d; }
            sPlanes[e] = vec4(n, d);
        }

        // View-space z depends only on NDC z for both projection kinds.
        sZNear = ViewPos(vec2(0.0), dMin * 2.0 - 1.0).z;
        sZFar  = ViewPos(vec2(0.0), dMax * 2.0 - 1.0).z;
    }
    barrier();

    if (!sEmpty) {
        for (uint i = lid; i < uint(uLightCount); i += 256u) {
            vec4 pr = lights[i].posRadius;
            float r = pr.w;
            if (r <= 0.0 || lights[i].colorIntensity.a <= 0.0) continue;   // switched off: adds nothing anyway

            vec3 v = (uView * vec4(pr.xyz, 1.0)).xyz;
            if (v.z - r > sZNear || v.z + r < sZFar) continue;

            bool hit = true;
            for (int e = 0; e < 4; e++)
                if (dot(sPlanes[e].xyz, v) + sPlanes[e].w < -r) { hit = false; break; }
            if (!hit) continue;

            uint slot = atomicAdd(sCount, 1u);
            if (slot < kMaxLights) sList[slot] = i;
        }
    }
    barrier();

    uint n = sEmpty ? 0u : min(sCount, kMaxLights);
    uint base = (tile.y * uint(uTilesX) + tile.x) * 128u;
    if (lid == 0u) lightGrid[base] = n;
    if (lid < 127u) {
        uint a = lid * 2u;
        uint lo = (a < n) ? sList[a] : 0u;
        uint hi = (a + 1u < n) ? sList[a + 1u] : 0u;
        lightGrid[base + 1u + lid] = lo | (hi << 16);
    }
}
)GLSL";
}
