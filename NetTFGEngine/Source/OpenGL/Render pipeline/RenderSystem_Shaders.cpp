#include "RenderSystem.hpp"

// =====================================================
//  CompileDepthShader
// =====================================================
void RenderSystem::CompileDepthShader()
{
    const char* vert = R"GLSL(
        #version 430 core
        layout(location = 0) in vec3 aPos;
        uniform mat4 uMVP;
        void main() { gl_Position = uMVP * vec4(aPos, 1.0); }
    )GLSL";

    const char* frag = R"GLSL(
        #version 430 core
        void main() {}
    )GLSL";

    m_depthShader = LinkProgram({
        CompileStage(GL_VERTEX_SHADER,   vert),
        CompileStage(GL_FRAGMENT_SHADER, frag)
        });
}

// =====================================================
//  CompileLightCullShader
// =====================================================
void RenderSystem::CompileLightCullShader()
{
    const char* comp = R"GLSL(
        #version 430 core
        layout(local_size_x = 16, local_size_y = 16) in;

        struct PointLight { vec4 posRadius; vec4 colorIntensity; };
        struct TileData   { uint offset; uint count; };

        layout(std430, binding = 0) readonly  buffer LightBuf  { PointLight lights[]; };
        layout(std430, binding = 1) writeonly buffer LightIdx   { uint lightIndices[]; };
        layout(std430, binding = 2)           buffer TileGrid   { TileData grid[]; };

        uniform sampler2D uDepthMap;
        uniform mat4      uProjection;
        uniform mat4      uView;
        uniform int       uLightCount;
        uniform ivec2     uScreenSize;

        shared uint s_minDepthInt;
        shared uint s_maxDepthInt;
        shared uint s_tileLightCount;
        shared uint s_tileLightIndices[256];

        vec4 CreatePlane(vec3 p0, vec3 p1, vec3 p2) {
            vec3 n = normalize(cross(p1 - p0, p2 - p0));
            return vec4(n, -dot(n, p0));
        }

        void main() {
            ivec2 tileID    = ivec2(gl_WorkGroupID.xy);
            ivec2 tileCount = ivec2(gl_NumWorkGroups.xy);
            int   tileIndex = tileID.y * tileCount.x + tileID.x;
            ivec2 pixel     = ivec2(gl_GlobalInvocationID.xy);

            if (gl_LocalInvocationIndex == 0u) {
                s_minDepthInt    = 0xFFFFFFFFu;
                s_maxDepthInt    = 0u;
                s_tileLightCount = 0u;
            }
            barrier();

            vec2 uv = (vec2(pixel) + 0.5) / vec2(uScreenSize);
            if (pixel.x < uScreenSize.x && pixel.y < uScreenSize.y) {
                float depth = texture(uDepthMap, uv).r;
                atomicMin(s_minDepthInt, floatBitsToUint(depth));
                atomicMax(s_maxDepthInt, floatBitsToUint(depth));
            }
            barrier();

            float minDepthVS = uintBitsToFloat(s_minDepthInt);
            float maxDepthVS = uintBitsToFloat(s_maxDepthInt);

            vec2 step  = 2.0 / vec2(tileCount);
            vec2 ndcLB = vec2(tileID) * step - 1.0;
            vec2 ndcRT = ndcLB + step;

            mat4 invProj = inverse(uProjection);
            vec3 lb = vec3((invProj * vec4(ndcLB.x, ndcLB.y, -1.0, 1.0)).xyz);
            vec3 rb = vec3((invProj * vec4(ndcRT.x, ndcLB.y, -1.0, 1.0)).xyz);
            vec3 lt = vec3((invProj * vec4(ndcLB.x, ndcRT.y, -1.0, 1.0)).xyz);
            vec3 rt = vec3((invProj * vec4(ndcRT.x, ndcRT.y, -1.0, 1.0)).xyz);
            vec3 origin = vec3(0.0);

            vec4 planes[4];
            planes[0] = CreatePlane(origin, lb, lt);
            planes[1] = CreatePlane(origin, rt, rb);
            planes[2] = CreatePlane(origin, rb, lb);
            planes[3] = CreatePlane(origin, lt, rt);

            for (int i = int(gl_LocalInvocationIndex); i < uLightCount; i += 256) {
                vec4  lightViewPos = uView * vec4(lights[i].posRadius.xyz, 1.0);
                float radius       = lights[i].posRadius.w;

                float lightMinZ = -lightViewPos.z - radius;
                float lightMaxZ = -lightViewPos.z + radius;
                if (lightMaxZ < minDepthVS || lightMinZ > maxDepthVS) continue;

                bool inside = true;
                for (int p = 0; p < 4; p++) {
                    if (dot(planes[p], vec4(lightViewPos.xyz, 1.0)) < -radius)
                        { inside = false; break; }
                }

                if (inside) {
                    uint slot = atomicAdd(s_tileLightCount, 1u);
                    if (slot < 256u)
                        s_tileLightIndices[slot] = uint(i);
                }
            }
            barrier();

            if (gl_LocalInvocationIndex == 0u) {
                uint offset = uint(tileIndex) * 256u;
                grid[tileIndex].offset = offset;
                grid[tileIndex].count  = min(s_tileLightCount, 256u);
                for (uint j = 0u; j < grid[tileIndex].count; j++)
                    lightIndices[offset + j] = s_tileLightIndices[j];
            }
        }
    )GLSL";

    m_lightCullShader = LinkProgram({ CompileStage(GL_COMPUTE_SHADER, comp) });
}

// =====================================================
//  CompileShadowShader
// =====================================================
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

// =====================================================
//  CompileTonemapShader
//  Reinhard and Filmic (Uncharted 2 / Hable) operators.
//  Gamma correction applied after both.
// =====================================================
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
        uniform sampler2D uSSAOTex;
        uniform sampler2D uBloomTex;
        uniform sampler2D uSSRTex;

        uniform float uExposure;
        uniform bool  uFilmicEnabled;
        uniform float uGamma;

        uniform bool  uSSAOEnabled;
        uniform bool  uBloomEnabled;
        uniform float uBloomStrength;
        uniform bool  uSSREnabled;

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

            if (uSSREnabled)
                hdrColor += texture(uSSRTex, vUV).rgb;

            if (uSSAOEnabled)
                hdrColor += vec3(0.03) * texture(uSSAOTex, vUV).r;
            else
                hdrColor += vec3(0.03);

            if (uBloomEnabled)
                hdrColor += texture(uBloomTex, vUV).rgb * uBloomStrength;

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