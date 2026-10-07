#include "RenderSystem.hpp"
#include "FSR_SDK/FSR1_GLSL.inl"
#include "NIS_SDK/NIS_Config.h"
#include "NIS_SDK/NIS_GLSL.inl"

// CompileShadowShader — point light cubemap
void RenderSystem::CompileShadowShader()
{
    const char* vert = R"GLSL(
        #version 430 core
        layout(location = 0) in vec3 aPos;
        uniform mat4 uModel;
        void main() { gl_Position = uModel * vec4(aPos, 1.0); }
    )GLSL";

    const char* geom = R"GLSL(
        #version 430 core
        layout(triangles) in;
        layout(triangle_strip, max_vertices = 18) out;

        uniform mat4 uLightSpaceMatrices[6];
        uniform int  uCubeArrayLayer;

        out vec4 gFragPos;

        void main() {
            for (int face = 0; face < 6; face++) {
                gl_Layer = uCubeArrayLayer + face;
                for (int v = 0; v < 3; v++) {
                    gFragPos    = gl_in[v].gl_Position;
                    gl_Position = uLightSpaceMatrices[face] * gFragPos;
                    EmitVertex();
                }
                EndPrimitive();
            }
        }
    )GLSL";

    const char* frag = R"GLSL(
        #version 430 core
        in  vec4  gFragPos;
        uniform vec3  uLightPos;
        uniform float uFarPlane;
        void main() {
            gl_FragDepth = length(gFragPos.xyz - uLightPos) / uFarPlane;
        }
    )GLSL";

    m_shadowShader = LinkProgram({
        CompileStage(GL_VERTEX_SHADER,   vert),
        CompileStage(GL_GEOMETRY_SHADER, geom),
        CompileStage(GL_FRAGMENT_SHADER, frag)
        });
}

// Simple depth-only pass for the directional light; no geometry shader needed since a single view-projection matrix transforms all geometry.
void RenderSystem::CompileDirShadowShader()
{
    const char* vert = R"GLSL(
        #version 430 core
        layout(location = 0) in vec3 aPos;
        uniform mat4 uModel;
        uniform mat4 uLightSpaceMatrix;
        void main()
        {
            gl_Position = uLightSpaceMatrix * uModel * vec4(aPos, 1.0);
        }
    )GLSL";

    // Depth is written implicitly — no fragment shader output needed.
    const char* frag = R"GLSL(
        #version 430 core
        void main() {}
    )GLSL";

    m_dirShadowShader = LinkProgram({
        CompileStage(GL_VERTEX_SHADER,   vert),
        CompileStage(GL_FRAGMENT_SHADER, frag)
        });
}

void RenderSystem::CompileTonemapShader()
{
    const char* vert = R"GLSL(
        #version 430 core
        layout(location = 0) in vec2 aPos;
        layout(location = 1) in vec2 aUV;
        out vec2 vUV;
        void main() { vUV = aUV; gl_Position = vec4(aPos, 0.0, 1.0); }
    )GLSL";

    const char* frag = R"GLSL(
        #version 430 core
        in  vec2 vUV;
        out vec4 FragColor;

        uniform sampler2D uHDRBuffer;
        uniform sampler2D uBloomTex;
        uniform bool      uBloomEnabled;
        uniform float     uBloomStrength;

        uniform float uExposure;
        uniform bool  uFilmicEnabled;
        uniform float uGamma;
        uniform float uA, uB, uC, uD, uE, uF, uW;

        vec3 FilmicCurve(vec3 x)
        {
            return ((x * (uA * x + uC * uB) + uD * uE)
                  / (x * (uA * x + uB)       + uD * uF))
                 - uE / uF;
        }

        void main()
        {
            vec3 hdrColor = texture(uHDRBuffer, vUV).rgb;

            if (uBloomEnabled)
                hdrColor += texture(uBloomTex, vUV).rgb * uBloomStrength;

            // inf (half-float overflow somewhere upstream) makes both curves inf/inf = NaN, i.e. a black pixel;
            // clamping keeps it white. A real NaN can't be recovered: made 0 explicitly rather than left to
            // clamp(NaN), which is undefined.
            hdrColor = any(isnan(hdrColor)) ? vec3(0.0) : min(hdrColor, vec3(60000.0));

            hdrColor *= uExposure;

            vec3 mapped;
            if (uFilmicEnabled) {
                vec3 whiteScale = vec3(1.0) / FilmicCurve(vec3(uW));
                mapped = FilmicCurve(hdrColor) * whiteScale;
            } else {
                mapped = hdrColor / (hdrColor + vec3(1.0));
            }

            mapped = pow(clamp(mapped, 0.0, 1.0), vec3(1.0 / uGamma));
            FragColor = vec4(mapped, 1.0);
        }
    )GLSL";

    m_tonemapShader = LinkProgram({
        CompileStage(GL_VERTEX_SHADER,   vert),
        CompileStage(GL_FRAGMENT_SHADER, frag)
        });
}

void RenderSystem::CompileBloomShaders()
{
    const char* fullscreenVert = R"GLSL(
        #version 430 core
        layout(location = 0) in vec2 aPos;
        layout(location = 1) in vec2 aUV;
        out vec2 vUV;
        void main() { vUV = aUV; gl_Position = vec4(aPos, 0.0, 1.0); }
    )GLSL";

    const char* threshFrag = R"GLSL(
        #version 430 core
        in  vec2 vUV;
        out vec4 FragColor;
        uniform sampler2D uHDRBuffer;
        uniform float     uThreshold;
        void main()
        {
            vec3 color = texture(uHDRBuffer, vUV).rgb;
            float brightness = dot(color, vec3(0.2126, 0.7152, 0.0722));
            FragColor = vec4((brightness > uThreshold) ? color : vec3(0.0), 1.0);
        }
    )GLSL";

    m_bloomThreshShader = LinkProgram({
        CompileStage(GL_VERTEX_SHADER,   fullscreenVert),
        CompileStage(GL_FRAGMENT_SHADER, threshFrag)
        });

    const char* kawaseFrag = R"GLSL(
        #version 430 core
        in  vec2 vUV;
        out vec4 FragColor;
        uniform sampler2D uTex;
        uniform vec2      uTexelSize;
        uniform int       uIteration;
        void main()
        {
            float offset = float(uIteration) + 0.5;
            vec3 sum = vec3(0.0);
            sum += texture(uTex, vUV + vec2(-offset, -offset) * uTexelSize).rgb;
            sum += texture(uTex, vUV + vec2( offset, -offset) * uTexelSize).rgb;
            sum += texture(uTex, vUV + vec2(-offset,  offset) * uTexelSize).rgb;
            sum += texture(uTex, vUV + vec2( offset,  offset) * uTexelSize).rgb;
            FragColor = vec4(sum * 0.25, 1.0);
        }
    )GLSL";

    m_bloomKawaseShader = LinkProgram({
        CompileStage(GL_VERTEX_SHADER,   fullscreenVert),
        CompileStage(GL_FRAGMENT_SHADER, kawaseFrag)
        });
}

void RenderSystem::CompileFXAAShader()
{
    const char* vert = R"GLSL(
        #version 430 core
        layout(location = 0) in vec2 aPos;
        layout(location = 1) in vec2 aUV;
        out vec2 vUV;
        void main() { vUV = aUV; gl_Position = vec4(aPos, 0.0, 1.0); }
    )GLSL";

    const char* frag = R"GLSL(
        #version 430 core
        in  vec2 vUV;
        out vec4 FragColor;

        uniform sampler2D uLDRBuffer;
        uniform vec2      uTexelSize;
        uniform float     uSubpix;
        uniform float     uEdgeThreshold;
        uniform float     uEdgeThresholdMin;

        float Luma(vec3 rgb) { return dot(rgb, vec3(0.299, 0.587, 0.114)); }

        void main()
        {
            vec2 uv = vUV;

            vec3  rgbM  = texture(uLDRBuffer, uv).rgb;
            float lumaM = Luma(rgbM);
            float lumaN = Luma(texture(uLDRBuffer, uv + vec2( 0, -1) * uTexelSize).rgb);
            float lumaS = Luma(texture(uLDRBuffer, uv + vec2( 0,  1) * uTexelSize).rgb);
            float lumaW = Luma(texture(uLDRBuffer, uv + vec2(-1,  0) * uTexelSize).rgb);
            float lumaE = Luma(texture(uLDRBuffer, uv + vec2( 1,  0) * uTexelSize).rgb);

            float rangeMin = min(lumaM, min(min(lumaN, lumaS), min(lumaW, lumaE)));
            float rangeMax = max(lumaM, max(max(lumaN, lumaS), max(lumaW, lumaE)));
            float range    = rangeMax - rangeMin;

            if (range < max(uEdgeThresholdMin, rangeMax * uEdgeThreshold)) {
                FragColor = vec4(rgbM, 1.0);
                return;
            }

            float lumaNW = Luma(texture(uLDRBuffer, uv + vec2(-1, -1) * uTexelSize).rgb);
            float lumaNE = Luma(texture(uLDRBuffer, uv + vec2( 1, -1) * uTexelSize).rgb);
            float lumaSW = Luma(texture(uLDRBuffer, uv + vec2(-1,  1) * uTexelSize).rgb);
            float lumaSE = Luma(texture(uLDRBuffer, uv + vec2( 1,  1) * uTexelSize).rgb);

            float edgeH = abs(lumaNW + 2.0*lumaN + lumaNE - lumaSW - 2.0*lumaS - lumaSE);
            float edgeV = abs(lumaNW + 2.0*lumaW + lumaSW - lumaNE - 2.0*lumaE - lumaSE);
            bool  isHorizontal = edgeH >= edgeV;

            float lumaAvg = (2.0*(lumaN+lumaS+lumaW+lumaE) +
                             lumaNW + lumaNE + lumaSW + lumaSE) / 12.0;
            float subpixBlend = clamp(abs(lumaAvg - lumaM) / range, 0.0, 1.0);
            subpixBlend = smoothstep(0.0, 1.0, subpixBlend) * uSubpix;

            vec2 blendDir = isHorizontal ? vec2(0.0, uTexelSize.y)
                                         : vec2(uTexelSize.x, 0.0);
            vec3 rgbBlend = 0.5 * (rgbM + texture(uLDRBuffer, uv + blendDir).rgb);

            FragColor = vec4(mix(rgbM, rgbBlend, subpixBlend), 1.0);
        }
    )GLSL";

    m_fxaaShader = LinkProgram({
        CompileStage(GL_VERTEX_SHADER,   vert),
        CompileStage(GL_FRAGMENT_SHADER, frag)
        });
}

// Screen-space effects run at a reduced resolution (uScale = 1, 2 or 4): reduced pixel p reads full pixel uScale*p of
// the GBuffer. The linear depth pass
// stores that pixel's view-space Z so SSAO/SSR samples are a single R32F fetch instead of fetch + inverse projection.
static const char* kFullscreenVert = R"GLSL(
    #version 430 core
    layout(location = 0) in vec2 aPos;
    layout(location = 1) in vec2 aUV;
    out vec2 vUV;
    void main() { vUV = aUV; gl_Position = vec4(aPos, 0.0, 1.0); }
)GLSL";

// SSAO (hemisphere kernel oriented by the GBuffer's view-space normal) + a depth-aware 4x4 blur, plus the linear
// depth pass both SSAO and SSR read.
void RenderSystem::CompileSSAOShaders()
{
    const char* linearDepthFrag = R"GLSL(
        #version 430 core
        out float FragZ;

        uniform sampler2D uDepthTex;      // full-res GBuffer depth
        uniform mat4      uInvProjection;
        uniform int       uScale;         // resolution divisor of this target

        void main()
        {
            ivec2 full = textureSize(uDepthTex, 0);
            ivec2 fp   = min(ivec2(gl_FragCoord.xy) * uScale, full - 1);
            float d    = texelFetch(uDepthTex, fp, 0).r;
            if (d >= 1.0) { FragZ = -1e6; return; } // background: behind everything, never a hit/occluder

            vec2 uv = (vec2(fp) + 0.5) / vec2(full);
            vec4 p  = uInvProjection * vec4(vec3(uv, d) * 2.0 - 1.0, 1.0);
            FragZ = p.z / p.w;
        }
    )GLSL";

    const char* ssaoFrag = R"GLSL(
        #version 430 core
        out float FragAO;

        uniform sampler2D uDepthTex;        // full-res GBuffer depth (centre position only)
        uniform sampler2D uNormalTex;       // full-res, xyz = view-space normal
        uniform sampler2D uLinearDepthTex;  // view-space Z at the SSAO resolution
        uniform int   uScale;               // SSAO resolution divisor
        uniform mat4  uProjection;
        uniform mat4  uInvProjection;
        uniform vec3  uKernel[64];
        uniform int   uKernelSize;
        uniform float uRadius;
        uniform float uBias;
        uniform float uIntensity;

        // 4x4 Bayer order: neighbouring pixels get well-spread rotations, and the blur averages exactly one tile.
        const float kBayer[16] = float[](0.0, 8.0, 2.0, 10.0, 12.0, 4.0, 14.0, 6.0,
                                         3.0, 11.0, 1.0, 9.0, 15.0, 7.0, 13.0, 5.0);

        void main()
        {
            ivec2 hp   = ivec2(gl_FragCoord.xy);
            ivec2 halfSize = textureSize(uLinearDepthTex, 0);
            ivec2 full = textureSize(uDepthTex, 0);
            ivec2 fp   = min(hp * uScale, full - 1);

            if (texelFetch(uLinearDepthTex, hp, 0).r < -1e5) { FragAO = 1.0; return; } // background

            // Centre position: the one inverse-projection of the pass.
            vec2 uv0 = (vec2(fp) + 0.5) / vec2(full);
            vec4 p   = uInvProjection * vec4(vec3(uv0, texelFetch(uDepthTex, fp, 0).r) * 2.0 - 1.0, 1.0);
            vec3 P   = p.xyz / p.w;
            vec3 N   = normalize(texelFetch(uNormalTex, fp, 0).xyz);

            ivec2 tile  = hp & 3;
            float angle = 6.2831853 * (kBayer[tile.y * 4 + tile.x] + 0.5) / 16.0;
            vec3  rv    = vec3(cos(angle), sin(angle), 0.0);

            vec3 T = rv - N * dot(rv, N);
            if (dot(T, T) < 1e-4) T = cross(N, vec3(0.0, 0.0, 1.0)); // N lies along rv
            T = normalize(T);
            mat3 TBN = mat3(T, cross(N, T), N);

            float occlusion = 0.0;
            float invSize   = 1.0 / float(uKernelSize);
            for (int i = 0; i < uKernelSize; ++i)
            {
                // Denser near the pixel: close geometry is what produces contact shadows.
                float s = float(i) * invSize;
                vec3  S = P + TBN * uKernel[i] * (mix(0.1, 1.0, s * s) * uRadius);

                vec4 clip = uProjection * vec4(S, 1.0);
                vec2 uv   = clip.xy / clip.w * 0.5 + 0.5;
                if (any(lessThan(uv, vec2(0.0))) || any(greaterThanEqual(uv, vec2(1.0)))) continue;

                float sceneZ = texelFetch(uLinearDepthTex, ivec2(uv * vec2(halfSize)), 0).r;

                // Fades out occluders far in front of P (e.g. a ship over the floor) instead of haloing them.
                float rangeCheck = smoothstep(0.0, 1.0, uRadius / max(abs(P.z - sceneZ), 1e-4));
                occlusion += (sceneZ >= S.z + uBias ? 1.0 : 0.0) * rangeCheck;
            }

            float ao = clamp(1.0 - occlusion * invSize, 0.0, 1.0);
            FragAO = pow(ao, uIntensity);
        }
    )GLSL";

    const char* blurFrag = R"GLSL(
        #version 430 core
        out float FragAO;

        uniform sampler2D uAOTex;
        uniform sampler2D uLinearDepthTex;
        uniform float uRadius;

        void main()
        {
            ivec2 size   = textureSize(uAOTex, 0);
            ivec2 center = ivec2(gl_FragCoord.xy);
            float zc     = texelFetch(uLinearDepthTex, center, 0).r;

            // 4x4 window = one full rotation tile. Samples across a depth discontinuity are dropped so AO from the
            // floor doesn't bleed onto the object standing on it (the centre always has weight 1).
            float sum = 0.0, wsum = 0.0;
            for (int y = -2; y < 2; ++y)
            for (int x = -2; x < 2; ++x)
            {
                ivec2 px = clamp(center + ivec2(x, y), ivec2(0), size - 1);
                float w  = max(0.0, 1.0 - abs(texelFetch(uLinearDepthTex, px, 0).r - zc) / uRadius);
                sum  += texelFetch(uAOTex, px, 0).r * w;
                wsum += w;
            }
            FragAO = sum / max(wsum, 1e-4);
        }
    )GLSL";

    m_linearDepthShader = LinkProgram({
        CompileStage(GL_VERTEX_SHADER,   kFullscreenVert),
        CompileStage(GL_FRAGMENT_SHADER, linearDepthFrag)
        });

    m_ssaoShader = LinkProgram({
        CompileStage(GL_VERTEX_SHADER,   kFullscreenVert),
        CompileStage(GL_FRAGMENT_SHADER, ssaoFrag)
        });

    m_ssaoBlurShader = LinkProgram({
        CompileStage(GL_VERTEX_SHADER,   kFullscreenVert),
        CompileStage(GL_FRAGMENT_SHADER, blurFrag)
        });

    // The kernel never changes, so it's uploaded once here rather than every frame.
    glUseProgram(m_ssaoShader);
    glUniform3fv(glGetUniformLocation(m_ssaoShader, "uKernel"),
        (GLsizei)m_ssaoKernel.size(), glm::value_ptr(m_ssaoKernel[0]));
    glUseProgram(0);
}

// SSR at half resolution: view-space linear march with per-pixel jitter against the half-res linear depth, refined by
// a binary search on the first crossing. The trace writes premultiplied colour + "replace" alpha into a half-res
// target; a full-res composite upscales it (depth-aware) and blends it onto the HDR scene: added, or replacing a
// material's own fallback reflection. Weighted by Fresnel and faded at screen edges, distance and roughness.
void RenderSystem::CompileSSRShader()
{
    const char* frag = R"GLSL(
        #version 430 core
        out vec4 FragColor;

        uniform sampler2D uSceneTex;        // half-res mipmapped HDR copy
        uniform sampler2D uDepthTex;        // full-res GBuffer depth (centre position only)
        uniform sampler2D uNormalTex;       // full-res, xyz = view-space normal, w = perceptual roughness
        uniform sampler2D uMaterialTex;     // full-res, r = metalness, g = own env reflection flag
        uniform sampler2D uAlbedoTex;
        uniform sampler2D uLinearDepthTex;  // view-space Z at the SSR resolution
        uniform int       uScale;           // SSR resolution divisor

        uniform mat4  uProjection;
        uniform mat4  uInvProjection;
        uniform int   uSteps;
        uniform float uMaxDistance;
        uniform float uThickness;
        uniform float uMaxRoughness;
        uniform float uIntensity;
        uniform float uMaxLod;

        ivec2 gHalf;

        vec2 Project(vec3 p)
        {
            vec4 c = uProjection * vec4(p, 1.0);
            return c.xy / c.w * 0.5 + 0.5;
        }

        float SceneZ(vec2 uv) { return texelFetch(uLinearDepthTex, clamp(ivec2(uv * vec2(gHalf)), ivec2(0), gHalf - 1), 0).r; }

        bool OnScreen(vec2 uv) { return all(greaterThanEqual(uv, vec2(0.0))) && all(lessThan(uv, vec2(1.0))); }

        void main()
        {
            gHalf = textureSize(uLinearDepthTex, 0);
            ivec2 hp   = ivec2(gl_FragCoord.xy);
            ivec2 full = textureSize(uDepthTex, 0);
            ivec2 fp   = min(hp * uScale, full - 1);

            float depth = texelFetch(uDepthTex, fp, 0).r;
            if (depth >= 1.0) discard;

            vec4  nr    = texelFetch(uNormalTex, fp, 0);
            float rough = nr.a;
            if (rough >= uMaxRoughness) discard;

            vec2 uv0 = (vec2(fp) + 0.5) / vec2(full);
            vec4 p   = uInvProjection * vec4(vec3(uv0, depth) * 2.0 - 1.0, 1.0);
            vec3 P   = p.xyz / p.w;
            vec3 N   = normalize(nr.xyz);
            vec3 V   = normalize(P);             // camera -> surface
            vec3 R   = normalize(reflect(V, N));

            // Fresnel and roughness fade first: they don't depend on the hit, so pixels whose reflection would be
            // invisible skip the whole march. Same F0 convention as the GGX shader; Schlick, roughness-limited peak.
            vec4  mat    = texelFetch(uMaterialTex, fp, 0);
            vec3  albedo = texelFetch(uAlbedoTex,   fp, 0).rgb;
            vec3  F0     = mix(vec3(0.04), albedo, mat.r);
            float NdotV  = clamp(dot(N, -V), 0.0, 1.0);
            vec3  F      = F0 + (max(vec3(1.0 - rough), F0) - F0) * pow(1.0 - NdotV, 5.0);
            vec3  w      = F * (1.0 - smoothstep(uMaxRoughness * 0.5, uMaxRoughness, rough)) * uIntensity;
            if (max(w.r, max(w.g, w.b)) < 0.005) discard;

            // Start slightly off the surface so the first steps don't hit the pixel itself.
            vec3  origin  = P + N * 0.05;
            float stepLen = uMaxDistance / float(uSteps);

            // Jittered start: turns step banding into noise that the roughness mip blur mostly hides.
            float jitter = fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))));

            float t = stepLen * jitter, prevT = 0.0;
            bool  hit = false;
            for (int i = 0; i < uSteps; ++i)
            {
                t += stepLen;
                vec3 pos = origin + R * t;
                if (pos.z > -0.01) break;        // behind the camera

                vec2 uv = Project(pos);
                if (!OnScreen(uv)) break;

                // > 0: the ray is behind the depth buffer. Within the thickness it's a hit; beyond it the ray is
                // just passing behind a foreground object and keeps going.
                float dz = SceneZ(uv) - pos.z;
                if (dz > 0.0 && dz < uThickness)
                {
                    float lo = prevT, hi = t;
                    for (int j = 0; j < 5; ++j)
                    {
                        float mid = 0.5 * (lo + hi);
                        vec3  m   = origin + R * mid;
                        if (SceneZ(Project(m)) - m.z > 0.0) hi = mid; else lo = mid;
                    }
                    t   = hi;
                    hit = true;
                    break;
                }
                prevT = t;
            }
            if (!hit) discard;

            vec2 hitUV = Project(origin + R * t);

            // The depth buffer only holds front faces: landing on a surface facing away from the ray means it went
            // behind something and the colour there is not what would be reflected. Faded, not discarded: with
            // normal-mapped GBuffer normals the test flips pixel by pixel and a hard cut leaves speckled holes.
            vec3  hitN   = textureLod(uNormalTex, hitUV, 0.0).xyz;
            float facing = dot(hitN, hitN) > 1e-6 ? dot(normalize(hitN), R) : 0.0;

            vec2  edge = smoothstep(0.0, 0.08, hitUV) * (1.0 - smoothstep(0.92, 1.0, hitUV));
            float fade = edge.x * edge.y;
            fade *= 1.0 - smoothstep(0.5, 1.0, t / uMaxDistance);
            fade *= 1.0 - smoothstep(0.0, 0.3, facing);

            vec3 wf   = w * fade;
            vec3 refl = textureLod(uSceneTex, hitUV, rough * uMaxLod).rgb;

            // Alpha 0 adds the reflection; for a material with its own fallback reflection, alpha removes that share
            // of the existing colour so the hit replaces it instead of doubling (see the composite's blend).
            float replace = mat.g * clamp(max(wf.r, max(wf.g, wf.b)), 0.0, 1.0);
            vec4  outC    = vec4(refl * wf, replace);

            // Whatever went wrong upstream (degenerate GBuffer normal...), never write NaN/inf into the HDR scene:
            // bloom would spread it into a black blob.
            if (any(isnan(outC)) || any(isinf(outC))) discard;
            FragColor = outC;
        }
    )GLSL";

    m_ssrShader = LinkProgram({
        CompileStage(GL_VERTEX_SHADER,   kFullscreenVert),
        CompileStage(GL_FRAGMENT_SHADER, frag)
        });

    // Scene -> half-res SSR copy. Bilinear at the centre of each 2x2 block = their average (the downsample for free).
    // Not a plain copy: glGenerateMipmap averages, so a single inf/NaN pixel (half-float overflow) would poison a
    // whole block at the blurrier mips, and a very hot pixel would smear into a bright square. NaN/inf are dropped
    // and luminance is capped (hue kept) before the mips are built.
    const char* copyFrag = R"GLSL(
        #version 430 core
        in  vec2 vUV;
        out vec4 FragColor;
        uniform sampler2D uHDRBuffer;
        uniform float     uMaxLuminance;
        void main()
        {
            vec3 c = texture(uHDRBuffer, vUV).rgb;
            if (any(isnan(c)) || any(isinf(c))) c = vec3(0.0);
            c = max(c, vec3(0.0));
            float lum = dot(c, vec3(0.2126, 0.7152, 0.0722));
            if (lum > uMaxLuminance) c *= uMaxLuminance / lum;
            FragColor = vec4(c, 1.0);
        }
    )GLSL";

    m_ssrCopyShader = LinkProgram({
        CompileStage(GL_VERTEX_SHADER,   kFullscreenVert),
        CompileStage(GL_FRAGMENT_SHADER, copyFrag)
        });

    // Full-res composite of the half-res trace. Plain bilinear would smear reflections across silhouettes (a floor
    // reflection bleeding onto the ship standing on it), so the 4 half-res neighbours are also weighted by how close
    // their depth is to this pixel's.
    const char* compositeFrag = R"GLSL(
        #version 430 core
        out vec4 FragColor;

        uniform sampler2D uTraceTex;        // SSR resolution, premultiplied colour + replace alpha
        uniform sampler2D uLinearDepthTex;  // view-space Z at the SSR resolution
        uniform sampler2D uDepthTex;        // full-res GBuffer depth
        uniform mat4      uInvProjection;
        uniform int       uScale;           // SSR resolution divisor

        void main()
        {
            ivec2 fp = ivec2(gl_FragCoord.xy);
            float d  = texelFetch(uDepthTex, fp, 0).r;
            if (d >= 1.0) discard;

            ivec2 full = textureSize(uDepthTex, 0);
            ivec2 halfSize = textureSize(uTraceTex, 0);
            vec2  uv   = (vec2(fp) + 0.5) / vec2(full);
            vec4  p    = uInvProjection * vec4(vec3(uv, d) * 2.0 - 1.0, 1.0);
            float z    = p.z / p.w;

            // Reduced pixel h covers full pixels s*h .. s*h+s-1: position of this pixel in reduced texel space.
            vec2  hc = (vec2(fp) + 0.5) / float(uScale) - 0.5;
            ivec2 h0 = ivec2(floor(hc));
            vec2  f  = hc - vec2(h0);
            float tolerance = 0.02 * abs(z) + 0.1;

            vec4  sum  = vec4(0.0);
            float wsum = 0.0;
            for (int y = 0; y < 2; ++y)
            for (int x = 0; x < 2; ++x)
            {
                ivec2 h  = clamp(h0 + ivec2(x, y), ivec2(0), halfSize - 1);
                float wb = (x == 0 ? 1.0 - f.x : f.x) * (y == 0 ? 1.0 - f.y : f.y);
                float wd = max(0.0, 1.0 - abs(texelFetch(uLinearDepthTex, h, 0).r - z) / tolerance);
                float w  = wb * wd + 1e-4 * wb; // tiny floor: an isolated pixel still gets the bilinear result
                sum  += texelFetch(uTraceTex, h, 0) * w;
                wsum += w;
            }
            vec4 c = sum / wsum;
            if (c.a <= 0.0 && dot(c.rgb, vec3(1.0)) <= 0.0) discard;
            FragColor = c;
        }
    )GLSL";

    m_ssrCompositeShader = LinkProgram({
        CompileStage(GL_VERTEX_SHADER,   kFullscreenVert),
        CompileStage(GL_FRAGMENT_SHADER, compositeFrag)
        });
}

// Motion blur, after McGuire et al. 2012 ("A Reconstruction Filter for Plausible Motion Blur"). Velocities are blur
// RADII in pixels (half the displacement during the exposure), clamped to one tile. Tile max + 3x3 neighbour max give
// every pixel the largest blur that can reach it, so a moving object also smears over the static background around
// it instead of only inside its own silhouette.
void RenderSystem::CompileMotionBlurShaders()
{
    // Camera motion by reprojecting the depth with last frame's view-projection, plus the object's own motion from the
    // GBuffer; scaled from "per frame" to the configured shutter time so the blur length doesn't depend on the FPS.
    const char* velocityFrag = R"GLSL(
        #version 430 core
        out vec2 FragVelocity;

        uniform sampler2D uDepthTex;           // full-res GBuffer depth
        uniform sampler2D uObjectVelocityTex;  // full-res, UV displacement from object motion
        uniform mat4  uInvViewProjection;
        uniform mat4  uPrevViewProjection;
        uniform float uExposureScale;          // shutter time / this frame's duration
        uniform float uMaxRadius;              // pixels (= tile size)

        void main()
        {
            ivec2 px   = ivec2(gl_FragCoord.xy);
            ivec2 size = textureSize(uDepthTex, 0);
            vec2  uv   = (vec2(px) + 0.5) / vec2(size);
            float d    = texelFetch(uDepthTex, px, 0).r;

            vec4 world = uInvViewProjection * vec4(vec3(uv, d) * 2.0 - 1.0, 1.0);
            world /= world.w;
            vec4 prev  = uPrevViewProjection * world;
            vec2 cam   = prev.w > 0.0 ? uv - (prev.xy / prev.w * 0.5 + 0.5) : vec2(0.0);

            vec2  motion = cam + texelFetch(uObjectVelocityTex, px, 0).xy;
            vec2  radius = motion * vec2(size) * (0.5 * uExposureScale);
            float len    = length(radius);
            if (len > uMaxRadius) radius *= uMaxRadius / len;
            FragVelocity = radius;
        }
    )GLSL";

    const char* tileMaxFrag = R"GLSL(
        #version 430 core
        out vec2 FragMax;
        uniform sampler2D uVelocityTex;
        uniform int       uTile;

        void main()
        {
            ivec2 size = textureSize(uVelocityTex, 0);
            ivec2 base = ivec2(gl_FragCoord.xy) * uTile;
            ivec2 end  = min(base + ivec2(uTile), size);

            vec2  best = vec2(0.0);
            float bestLen2 = 0.0;
            for (int y = base.y; y < end.y; ++y)
            for (int x = base.x; x < end.x; ++x)
            {
                vec2  v = texelFetch(uVelocityTex, ivec2(x, y), 0).xy;
                float l = dot(v, v);
                if (l > bestLen2) { bestLen2 = l; best = v; }
            }
            FragMax = best;
        }
    )GLSL";

    const char* neighborMaxFrag = R"GLSL(
        #version 430 core
        out vec2 FragMax;
        uniform sampler2D uTileMaxTex;

        void main()
        {
            ivec2 size = textureSize(uTileMaxTex, 0);
            ivec2 t    = ivec2(gl_FragCoord.xy);

            vec2  best = vec2(0.0);
            float bestLen2 = 0.0;
            for (int y = -1; y <= 1; ++y)
            for (int x = -1; x <= 1; ++x)
            {
                vec2  v = texelFetch(uTileMaxTex, clamp(t + ivec2(x, y), ivec2(0), size - 1), 0).xy;
                float l = dot(v, v);
                if (l > bestLen2) { bestLen2 = l; best = v; }
            }
            FragMax = best;
        }
    )GLSL";

    const char* gatherFrag = R"GLSL(
        #version 430 core
        out vec4 FragColor;

        uniform sampler2D uSourceTex;       // copy of the HDR scene
        uniform sampler2D uVelocityTex;     // full-res blur radius (px)
        uniform sampler2D uNeighborMaxTex;  // per tile: largest radius that can reach it
        uniform sampler2D uLinearDepthTex;  // view-space Z at the SSAO resolution (background = -1e6)
        uniform int       uDepthScale;      // its resolution divisor
        uniform int       uTile;
        uniform int       uSamples;

        // Depth range (world units) over which "in front" / "behind" blend instead of switching hard.
        const float kSoftZ = 1.0;

        float Dist(ivec2 p)
        {
            ivec2 depthSize = textureSize(uLinearDepthTex, 0);
            return -texelFetch(uLinearDepthTex, min(p / uDepthScale, depthSize - 1), 0).r;
        }

        // 1 when a is closer to the camera than (or level with) b.
        float Closer(float a, float b) { return clamp(1.0 - (a - b) / kSoftZ, 0.0, 1.0); }
        // Does a blur of radius r starting at one point reach a point dist pixels away?
        float Cone(float dist, float r)     { return clamp(1.0 - dist / r, 0.0, 1.0); }
        float Cylinder(float dist, float r) { return 1.0 - smoothstep(0.95 * r, 1.05 * r, dist); }

        void main()
        {
            ivec2 X    = ivec2(gl_FragCoord.xy);
            ivec2 size = textureSize(uSourceTex, 0);

            vec2 vn = texelFetch(uNeighborMaxTex, X / uTile, 0).xy;
            if (dot(vn, vn) < 0.25) discard; // nothing moves near here: the scene is already right

            vec2  vX    = texelFetch(uVelocityTex, X, 0).xy;
            float rX    = max(length(vX), 0.5);
            float zX    = Dist(X);
            vec2  pX    = vec2(X) + 0.5;
            vec2  invSz = 1.0 / vec2(size);

            // The pixel itself, weighted so a sharp (slow) pixel keeps most of its own colour.
            float wsum = 1.0 / rX;
            vec4  sum  = texture(uSourceTex, pX * invSz) * wsum;

            // Per-pixel jitter of the sample positions: banding becomes fine noise.
            float jitter = fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715)))) - 0.5;

            for (int i = 0; i < uSamples; ++i)
            {
                float t    = mix(-1.0, 1.0, (float(i) + 1.0 + jitter) / (float(uSamples) + 1.0));
                vec2  pY   = pX + vn * t;
                ivec2 Y    = clamp(ivec2(pY), ivec2(0), size - 1);
                float dist = length(vn * t);

                float rY = max(length(texelFetch(uVelocityTex, Y, 0).xy), 0.5);
                float zY = Dist(Y);

                // Y in front and blurred over X | Y behind, seen through X's own blur | both blurred together.
                float a = Closer(zY, zX) * Cone(dist, rY)
                        + Closer(zX, zY) * Cone(dist, rX)
                        + Cylinder(dist, rY) * Cylinder(dist, rX) * 2.0;

                sum  += texture(uSourceTex, pY * invSz) * a;
                wsum += a;
            }

            FragColor = vec4((sum / wsum).rgb, 1.0);
        }
    )GLSL";

    m_mbVelocityShader = LinkProgram({
        CompileStage(GL_VERTEX_SHADER,   kFullscreenVert),
        CompileStage(GL_FRAGMENT_SHADER, velocityFrag)
        });
    m_mbTileMaxShader = LinkProgram({
        CompileStage(GL_VERTEX_SHADER,   kFullscreenVert),
        CompileStage(GL_FRAGMENT_SHADER, tileMaxFrag)
        });
    m_mbNeighborMaxShader = LinkProgram({
        CompileStage(GL_VERTEX_SHADER,   kFullscreenVert),
        CompileStage(GL_FRAGMENT_SHADER, neighborMaxFrag)
        });
    m_mbGatherShader = LinkProgram({
        CompileStage(GL_VERTEX_SHADER,   kFullscreenVert),
        CompileStage(GL_FRAGMENT_SHADER, gatherFrag)
        });
}

void RenderSystem::CompileGBufferShader()
{
    const char* vert = R"GLSL(
        #version 430 core
        layout(location = 0) in vec3 aPos;
        layout(location = 1) in vec3 aNormal;
        layout(location = 2) in vec2 aUV;
        layout(location = 3) in vec4 aTangent;

        uniform mat4 uModelViewProjection;
        uniform mat4 uPrevModelViewProjection; // this frame's view-projection * last frame's model: object motion only
        uniform mat3 uViewNormalMatrix;   // transpose(inverse(view * model)), per mesh on the CPU

        out vec2 vUV;
        out mat3 vViewTBN;
        out vec4 vCurrClip;
        out vec4 vPrevClip;

        void main()
        {
            mat3 viewNormalMatrix = uViewNormalMatrix;

            vec3 vN = normalize(viewNormalMatrix * aNormal);
            vec3 vT = normalize(viewNormalMatrix * aTangent.xyz);
            vT      = normalize(vT - dot(vT, vN) * vN);
            vec3 vB = cross(vN, vT) * aTangent.w;
            vViewTBN = mat3(vT, vB, vN);

            vUV         = aUV;
            gl_Position = uModelViewProjection * vec4(aPos, 1.0);
            vCurrClip   = gl_Position;
            vPrevClip   = uPrevModelViewProjection * vec4(aPos, 1.0);
        }
    )GLSL";

    const char* frag = R"GLSL(
        #version 430 core
        in vec2 vUV;
        in mat3 vViewTBN;
        in vec4 vCurrClip;
        in vec4 vPrevClip;

        // Layout shared with GBufferVariant.hpp's preamble.
        layout(location = 0) out vec4 FragNormalRoughness;
        layout(location = 1) out vec4 FragMaterial;
        layout(location = 2) out vec4 FragAlbedo;
        layout(location = 3) out vec2 FragObjectVelocity;

        uniform sampler2D uAlbedoTex;
        uniform sampler2D uNormalTex;
        uniform sampler2D uMRTex;

        void main()
        {
            vec3 tangentN   = texture(uNormalTex, vUV).rgb * 2.0 - 1.0;
            vec3 viewNormal = normalize(vViewTBN * tangentN);

            vec2 mr = texture(uMRTex, vUV).gb;
            float perceptualRoughness = clamp(mr.x, 0.045, 1.0);
            float metalness           = clamp(mr.y, 0.0,   1.0);

            FragNormalRoughness = vec4(viewNormal, perceptualRoughness);
            // g = 0: the default material has no env reflection of its own, so SSR adds on top (GBufferVariant.hpp).
            FragMaterial        = vec4(metalness, 0.0, 0.0, 1.0);
            FragAlbedo          = vec4(texture(uAlbedoTex, vUV).rgb, 1.0);
            // UV-space displacement since last frame from the object's own movement (camera motion is added later
            // by reprojecting the depth, so static geometry needn't be tracked).
            FragObjectVelocity  = (vCurrClip.xy / vCurrClip.w - vPrevClip.xy / vPrevClip.w) * 0.5;
        }
    )GLSL";

    m_gbufferShader = LinkProgram({
        CompileStage(GL_VERTEX_SHADER,   vert),
        CompileStage(GL_FRAGMENT_SHADER, frag)
        });
}

// AMD FidelityFX FSR 1, from the SDK headers in FSR_SDK (embedded by Scripts/embed_fsr_sdk.py). Both passes use the
// SDK's 32-bit paths (FSR_EASU_F / FSR_RCAS_F) and only provide the input callbacks it asks for; the constants come
// from FsrEasuCon/FsrRcasCon on the CPU (FSRPass). The output pixel is gl_FragCoord: GL's bottom-left origin is the
// same for the input texture and the target, so the image is just processed vertically mirrored, consistently.
// On a compile/link failure the program stays 0 and ActiveUpscaleMode() falls back to Bilinear.
void RenderSystem::CompileFSRShaders()
{
    glDeleteProgram(m_fsrEasuShader); m_fsrEasuShader = 0;
    glDeleteProgram(m_fsrRcasShader); m_fsrRcasShader = 0;

    const std::string header =
        "#version 430 core\n"
        "#define A_GPU 1\n"
        "#define A_GLSL 1\n"
        + FSR1_GLSL::FfxA();

    // EASU: textureGather of each channel around p, as the SDK's GLSL example does.
    const std::string easuFrag = header +
        "#define FSR_EASU_F 1\n"
        + FSR1_GLSL::FfxFsr1() + R"GLSL(
        uniform sampler2D uInput;
        uniform uvec4 uCon0;
        uniform uvec4 uCon1;
        uniform uvec4 uCon2;
        uniform uvec4 uCon3;
        out vec4 FragColor;

        AF4 FsrEasuRF(AF2 p) { return textureGather(uInput, p, 0); }
        AF4 FsrEasuGF(AF2 p) { return textureGather(uInput, p, 1); }
        AF4 FsrEasuBF(AF2 p) { return textureGather(uInput, p, 2); }

        void main()
        {
            AF3 c;
            FsrEasuF(c, AU2(gl_FragCoord.xy), uCon0, uCon1, uCon2, uCon3);
            FragColor = vec4(c, 1.0);
        }
    )GLSL";

    // RCAS: reads the EASU output texel by texel; the input is already in display (gamma) space, no conversion.
    const std::string rcasFrag = header +
        "#define FSR_RCAS_F 1\n"
        + FSR1_GLSL::FfxFsr1() + R"GLSL(
        uniform sampler2D uInput;
        uniform uvec4 uCon;
        out vec4 FragColor;

        AF4  FsrRcasLoadF(ASU2 p) { return texelFetch(uInput, p, 0); }
        void FsrRcasInputF(inout AF1 r, inout AF1 g, inout AF1 b) {}

        void main()
        {
            AF1 r, g, b;
            FsrRcasF(r, g, b, AU2(gl_FragCoord.xy), uCon);
            FragColor = vec4(r, g, b, 1.0);
        }
    )GLSL";

    auto build = [](const std::string& frag) -> GLuint {
        GLuint prog = LinkProgram({
            CompileStage(GL_VERTEX_SHADER,   kFullscreenVert),
            CompileStage(GL_FRAGMENT_SHADER, frag.c_str())
            });
        GLint ok = 0;
        glGetProgramiv(prog, GL_LINK_STATUS, &ok);
        if (!ok) { glDeleteProgram(prog); return 0; }
        return prog;
    };

    m_fsrEasuShader = build(easuFrag);
    m_fsrRcasShader = build(rcasFrag);
    if (!m_fsrEasuShader || !m_fsrRcasShader)
        Debug::Error("RenderSystem") << "FSR 1 shaders failed to build; upscaling falls back to bilinear\n";
}

// NVIDIA Image Scaling (NVScaler) from the SDK in NIS_SDK: NIS_Scaler.h embedded by Scripts/embed_nis_sdk.py (texture
// macros patched to plain sampler2D uniforms), coefficient tables and block sizes from NIS_Config.h. A compute shader,
// so it needs GL 4.3 (the context is only asked for 3.3): without it, or on a compile/link failure, m_nisShader stays 0
// and ActiveUpscaleMode() falls back to Bilinear. Like FSR, GL's bottom-left origin is the same for input and output.
void RenderSystem::CompileNISShader()
{
    DeleteNIS();

    if (!glDispatchCompute || !glBindImageTexture || !glMemoryBarrier || !glBindSampler) {
        Debug::Warning("RenderSystem") << "No compute shader support (GL 4.3); NIS upscaling falls back to bilinear\n";
        return;
    }

    // Block and thread group size tuned per vendor by the SDK.
    const char* vendor = reinterpret_cast<const char*>(glGetString(GL_VENDOR));
    const std::string v = vendor ? vendor : "";
    NISGPUArchitecture arch = NISGPUArchitecture::NVIDIA_Generic;
    if (v.find("AMD") != std::string::npos || v.find("ATI") != std::string::npos) arch = NISGPUArchitecture::AMD_Generic;
    else if (v.find("Intel") != std::string::npos)                                 arch = NISGPUArchitecture::Intel_Generic;
    NISOptimizer opt(true, arch);
    m_nisBlockW = static_cast<int>(opt.GetOptimalBlockWidth());
    m_nisBlockH = static_cast<int>(opt.GetOptimalBlockHeight());
    const int groupSize = static_cast<int>(opt.GetOptimalThreadGroupSize());

    // Every NIS_* option the header tests is defined explicitly (an undefined name in #if isn't portable in GLSL).
    // The constants block mirrors NISConfig: std140 packs scalars at 4 bytes like the C++ struct.
    std::string src =
        "#version 430 core\n"
        "#define NIS_GLSL 1\n"
        "#define NIS_HLSL 0\n"
        "#define NIS_HLSL_6_2 0\n"
        "#define NIS_SCALER 1\n"
        "#define NIS_HDR_MODE 0\n"
        "#define NIS_VIEWPORT_SUPPORT 0\n"
        "#define NIS_NV12_SUPPORT 0\n"
        "#define NIS_CLAMP_OUTPUT 1\n"
        "#define NIS_USE_HALF_PRECISION 0\n"
        "#define NIS_TEXTURE_GATHER 0\n"
        "#define NIS_BLOCK_WIDTH " + std::to_string(m_nisBlockW) + "\n"
        "#define NIS_BLOCK_HEIGHT " + std::to_string(m_nisBlockH) + "\n"
        "#define NIS_THREAD_GROUP_SIZE " + std::to_string(groupSize) + "\n"
        R"GLSL(
        layout(std140, binding = 3) uniform NISConstants
        {
            float kDetectRatio;
            float kDetectThres;
            float kMinContrastRatio;
            float kRatioNorm;

            float kContrastBoost;
            float kEps;
            float kSharpStartY;
            float kSharpScaleY;

            float kSharpStrengthMin;
            float kSharpStrengthScale;
            float kSharpLimitMin;
            float kSharpLimitScale;

            float kScaleX;
            float kScaleY;

            float kDstNormX;
            float kDstNormY;
            float kSrcNormX;
            float kSrcNormY;

            uint kInputViewportOriginX;
            uint kInputViewportOriginY;
            uint kInputViewportWidth;
            uint kInputViewportHeight;

            uint kOutputViewportOriginX;
            uint kOutputViewportOriginY;
            uint kOutputViewportWidth;
            uint kOutputViewportHeight;

            float reserved0;
            float reserved1;
        };

        layout(binding = 0) uniform sampler2D in_texture;
        layout(binding = 1) uniform sampler2D coef_scaler;
        layout(binding = 2) uniform sampler2D coef_usm;
        layout(binding = 0, rgba8) uniform writeonly image2D out_texture;
        )GLSL"
        + NIS_GLSL::NisScaler() + R"GLSL(
        layout(local_size_x = NIS_THREAD_GROUP_SIZE) in;
        void main()
        {
            NVScaler(gl_WorkGroupID.xy, gl_LocalInvocationID.x);
        }
        )GLSL";

    // Bounds-checked stores. With NIS_VIEWPORT_SUPPORT 0 the SDK writes every pixel of every dispatched block, and the
    // dispatch is rounded up to whole blocks (1366 wide / 32 = 43 blocks -> columns up to 1375): those stores fall
    // outside out_texture. GL says they have no effect and NVIDIA honours that; the Radeon 520's 21.19 driver showed
    // garbage blocks over the whole image and then crashed inside the driver in unrelated draws. Patched here rather
    // than in the generated NIS_GLSL.inl, so re-running Scripts/embed_nis_sdk.py keeps it.
    {
        const std::string store = "#define NVTEX_STORE(x, pos, v) imageStore(x, NVI2(pos), v)";
        const size_t at = src.find(store);
        if (at != std::string::npos)
            src.replace(at, store.size(),
                "#define NVTEX_STORE(x, pos, v) { if (all(lessThan(uvec2(NVI2(pos)), uvec2(imageSize(x))))) imageStore(x, NVI2(pos), v); }");
        else
            Debug::Warning("RenderSystem") << "NIS: NVTEX_STORE not found in the SDK source, stores stay unchecked\n";
    }

    GLuint prog = LinkProgram({ CompileStage(GL_COMPUTE_SHADER, src.c_str()) });
    GLint ok = 0;
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        glDeleteProgram(prog);
        Debug::Error("RenderSystem") << "NIS shader failed to build; upscaling falls back to bilinear\n";
        return;
    }
    m_nisShader = prog;

    // Coefficients: 8 taps per phase = 2 RGBA texels per row, one row per phase; the shader texelFetches them.
    auto makeCoefTex = [](const float (&coef)[kPhaseCount][kFilterSize]) {
        GLuint tex = 0;
        glGenTextures(1, &tex);
        glBindTexture(GL_TEXTURE_2D, tex);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, static_cast<GLsizei>(kFilterSize / 4),
            static_cast<GLsizei>(kPhaseCount), 0, GL_RGBA, GL_FLOAT, coef);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        return tex;
    };
    m_nisCoefScaleTex = makeCoefTex(coef_scale);
    m_nisCoefUsmTex = makeCoefTex(coef_usm);
    glBindTexture(GL_TEXTURE_2D, 0);

    glGenBuffers(1, &m_nisConfigUBO);
    glBindBuffer(GL_UNIFORM_BUFFER, m_nisConfigUBO);
    glBufferData(GL_UNIFORM_BUFFER, sizeof(NISConfig), nullptr, GL_DYNAMIC_DRAW);
    glBindBuffer(GL_UNIFORM_BUFFER, 0);

    // The SDK samples the input with a linear/clamp sampler (samplerLinearClamp).
    glGenSamplers(1, &m_nisSampler);
    glSamplerParameteri(m_nisSampler, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glSamplerParameteri(m_nisSampler, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glSamplerParameteri(m_nisSampler, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glSamplerParameteri(m_nisSampler, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
}

void RenderSystem::DeleteNIS()
{
    glDeleteProgram(m_nisShader);            m_nisShader = 0;
    glDeleteTextures(1, &m_nisCoefScaleTex); m_nisCoefScaleTex = 0;
    glDeleteTextures(1, &m_nisCoefUsmTex);   m_nisCoefUsmTex = 0;
    glDeleteBuffers(1, &m_nisConfigUBO);     m_nisConfigUBO = 0;
    if (glDeleteSamplers) glDeleteSamplers(1, &m_nisSampler);
    m_nisSampler = 0;
}

// Plain copy of a render-resolution image to the current target at its size: bilinear (texture()) or nearest-neighbour
// (the one source texel each target pixel falls in). Used instead of glBlitFramebuffer to reach the window: a blit
// into the default framebuffer is INVALID_OPERATION when it is multisampled (e.g. AA forced in the driver) and the
// sizes or formats differ, and then the window was never written nor cleared.
void RenderSystem::CompileCopyShader()
{
    glDeleteProgram(m_copyShader);

    const char* frag = R"GLSL(
        #version 430 core
        in  vec2 vUV;
        out vec4 FragColor;
        uniform sampler2D uTex;
        uniform int       uNearest;
        void main()
        {
            vec3 c;
            if (uNearest != 0) {
                ivec2 size = textureSize(uTex, 0);
                c = texelFetch(uTex, clamp(ivec2(vUV * vec2(size)), ivec2(0), size - 1), 0).rgb;
            } else {
                c = texture(uTex, vUV).rgb;
            }
            FragColor = vec4(c, 1.0);
        }
    )GLSL";

    m_copyShader = LinkProgram({
        CompileStage(GL_VERTEX_SHADER,   kFullscreenVert),
        CompileStage(GL_FRAGMENT_SHADER, frag)
        });
}