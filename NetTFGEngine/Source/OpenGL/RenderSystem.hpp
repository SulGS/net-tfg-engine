#ifndef RENDER_SYSTEM_HPP
#define RENDER_SYSTEM_HPP

#include "ecs/ecs.hpp"
#include "ecs/ecs_common.hpp"
#include "Utils/Debug/Debug.hpp"
#include "Mesh.hpp"
#include "Material.hpp"
#include "OpenGLIncludes.hpp"
#include "RenderSettings.hpp"
#include <glm/gtc/type_ptr.hpp>
#include <vector>
#include <string>
#include <iostream>
#include <cstdlib>
#include <cmath>
#include <algorithm>

// -------------------------------------------------------
//  GPU-side light layout — must match light_cull.comp
//  and the SSBO declarations in every .frag shader
// -------------------------------------------------------
struct GPUPointLight {
    glm::vec4 posRadius;      // xyz = world pos,  w = radius
    glm::vec4 colorIntensity; // rgb = color,       a = intensity
};

struct GPUTileData {
    uint32_t offset;
    uint32_t count;
};

struct GPUShadowData {
    glm::mat4 lightSpaceMatrices[6]; // una por cara del cubo
    int       lightIndex;
    float     farPlane;
    int       pad[2];
};

// -------------------------------------------------------
//  Forward+ RenderSystem
//
//  Per-frame pipeline:
//    1. UploadLights   — stream PointLightComponent data → SSBO
//    2. DepthPrePass   — depth-only draw to offscreen FBO
//    3. LightCullPass  — compute shader tiles the screen,
//                        culls lights per tile
//    4. ShadingPass    — normal mesh draw; every frag shader
//                        reads the tile list from SSBOs
// -------------------------------------------------------
class RenderSystem : public ISystem {
public:

    // ---------------------------------------------------
    //  Call once after the OpenGL context is ready.
    //  Reads init-time constants from RenderSettings.
    //  screenW / screenH must match your actual viewport.
    // ---------------------------------------------------
    void Init(int screenW, int screenH)
    {
        // ---- Read init-time settings once ----
        const auto& rs = RenderSettings::instance();
        MAX_LIGHTS = rs.getMaxLights();
        MAX_LIGHTS_TILE = rs.getMaxLightsPerTile();
        TILE_SIZE = rs.getTileSize();
        MAX_SHADOW_LIGHTS = rs.getMaxShadowLights();

        m_screenW = screenW;
        m_screenH = screenH;
        m_tilesX = (screenW + TILE_SIZE - 1) / TILE_SIZE;
        m_tilesY = (screenH + TILE_SIZE - 1) / TILE_SIZE;

        InitDepthFBO();
        InitSSBOs();
        InitShadowCubeArray();
        InitHDRFBO();
        InitSSAO();
        InitBloom();
        InitSSR();
        InitFXAA();
        CompileDepthShader();
        CompileLightCullShader();
        CompileShadowShader();
        CompileTonemapShader();
        CompileSSAOShaders();
        CompileBloomShaders();
        CompileSSRShader();
        CompileFXAAShader();
        InitScreenQuad();
    }

    // ---------------------------------------------------
    //  Call when the window is resized.
    // ---------------------------------------------------
    void Resize(int screenW, int screenH)
    {
        m_screenW = screenW;
        m_screenH = screenH;
        m_tilesX = (screenW + TILE_SIZE - 1) / TILE_SIZE;
        m_tilesY = (screenH + TILE_SIZE - 1) / TILE_SIZE;

        // --- Depth FBO (screen-size dependent) ---
        glDeleteFramebuffers(1, &m_depthFBO); m_depthFBO = 0;
        glDeleteTextures(1, &m_depthTex);     m_depthTex = 0;
        InitDepthFBO();

        // --- HDR FBO (screen-size dependent) ---
        glDeleteFramebuffers(1, &m_hdrFBO);   m_hdrFBO = 0;
        glDeleteTextures(1, &m_hdrColorTex);  m_hdrColorTex = 0;
        glDeleteTextures(1, &m_hdrDepthTex);  m_hdrDepthTex = 0;
        InitHDRFBO();

        // --- SSAO FBOs (screen-size dependent) ---
        glDeleteFramebuffers(1, &m_ssaoFBO);     m_ssaoFBO = 0;
        glDeleteFramebuffers(1, &m_ssaoBlurFBO); m_ssaoBlurFBO = 0;
        glDeleteTextures(1, &m_ssaoTex);         m_ssaoTex = 0;
        glDeleteTextures(1, &m_ssaoBlurTex);     m_ssaoBlurTex = 0;
        InitSSAO();

        // --- Bloom FBOs (screen-size dependent) ---
        glDeleteFramebuffers(1, &m_bloomThreshFBO); m_bloomThreshFBO = 0;
        glDeleteFramebuffers(1, &m_bloomPingFBO);   m_bloomPingFBO = 0;
        glDeleteFramebuffers(1, &m_bloomPongFBO);   m_bloomPongFBO = 0;
        glDeleteTextures(1, &m_bloomThreshTex);     m_bloomThreshTex = 0;
        glDeleteTextures(1, &m_bloomPingTex);       m_bloomPingTex = 0;
        glDeleteTextures(1, &m_bloomPongTex);       m_bloomPongTex = 0;
        InitBloom();

        // --- SSR FBO (screen-size dependent) ---
        glDeleteFramebuffers(1, &m_ssrFBO); m_ssrFBO = 0;
        glDeleteTextures(1, &m_ssrTex);     m_ssrTex = 0;
        InitSSR();

        // --- FXAA FBO (screen-size dependent) ---
        glDeleteFramebuffers(1, &m_fxaaFBO); m_fxaaFBO = 0;
        glDeleteTextures(1, &m_fxaaTex);     m_fxaaTex = 0;
        InitFXAA();

        // --- Only the tile-dependent SSBOs need rebuilding ---
        // m_lightSSBO is MAX_LIGHTS fixed size — do NOT touch it
        int totalTiles = m_tilesX * m_tilesY;

        glDeleteBuffers(1, &m_lightIndexSSBO); m_lightIndexSSBO = 0;
        glGenBuffers(1, &m_lightIndexSSBO);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, m_lightIndexSSBO);
        glBufferData(GL_SHADER_STORAGE_BUFFER,
            sizeof(uint32_t) * totalTiles * MAX_LIGHTS_TILE,
            nullptr, GL_DYNAMIC_DRAW);

        glDeleteBuffers(1, &m_tileGridSSBO); m_tileGridSSBO = 0;
        glGenBuffers(1, &m_tileGridSSBO);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, m_tileGridSSBO);
        glBufferData(GL_SHADER_STORAGE_BUFFER,
            sizeof(GPUTileData) * totalTiles,
            nullptr, GL_DYNAMIC_DRAW);

        glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
    }

    // ---------------------------------------------------
    //  Re-creates the shadow cubemap array using the
    //  current RenderSettings::getShadowResolution().
    //  Call this after changing shadow resolution at runtime.
    // ---------------------------------------------------
    void ReInitShadows()
    {
        glDeleteFramebuffers(1, &m_shadowFBO);       m_shadowFBO = 0;
        glDeleteTextures(1, &m_shadowCubeArray);     m_shadowCubeArray = 0;
        glDeleteBuffers(1, &m_shadowDataSSBO);        m_shadowDataSSBO = 0;
        InitShadowCubeArray();
    }

    void Update(EntityManager& entityManager,
        std::vector<EventEntry>& events,
        bool isServer,
        float deltaTime) override
    {
        if (m_depthFBO == 0) {
            Debug::Error("RenderSystem") << "Call Init() before first Update()\n";
            return;
        }

        // --------------- acquire camera ---------------
        Camera* activeCamera = nullptr;
        Transform* cameraTransform = nullptr;

        entityManager.acquireMutex();

        auto cameraQuery = entityManager.CreateQuery<Camera, Transform>();
        for (auto [entity, camera, transform] : cameraQuery) {
            activeCamera = camera;
            cameraTransform = transform;
            break;
        }

        if (!activeCamera || !cameraTransform) {
            Debug::Warning("RenderSystem") << "No camera found\n";
            entityManager.releaseMutex();
            return;
        }

        glm::mat4 view = activeCamera->getViewMatrix();
        glm::mat4 projection = activeCamera->getProjectionMatrix();
        glm::vec3 cameraPos = cameraTransform->getPosition();

        auto meshQuery = entityManager.CreateQuery<MeshComponent, Transform>();

        // ---- Pass 1: upload lights to SSBO ----
        UploadLights(entityManager);

        // ---- Pass 2 (optional): shadow pass ----
        if (RenderSettings::instance().getShadowsEnabled())
            ShadowPass(entityManager, meshQuery);
        else
            m_shadowCount = 0;

        // ---- Pass 3: depth pre-pass ----
        DepthPrePass(meshQuery, view, projection);

        // ---- Pass 4: light culling compute ----
        LightCullPass(view, projection);

        // ---- Pass 5: full shading (into HDR FBO) ----
        ShadingPass(meshQuery, view, projection, cameraPos);

        // ---- Pass 6: SSAO (reads depth, writes occlusion tex) ----
        if (RenderSettings::instance().getSSAOEnabled())
            SSAOPass(projection);

        // ---- Pass 7: SSR (reads depth+colour, writes reflection tex) ----
        if (RenderSettings::instance().getSSREnabled())
            SSRPass(view, projection);

        // ---- Pass 8: Bloom (threshold + Kawase blur in HDR) ----
        if (RenderSettings::instance().getBloomEnabled())
            BloomPass();

        // ---- Pass 9: tonemap HDR + composite SSAO/SSR/Bloom → LDR FBO ----
        TonemapPass();

        // ---- Pass 10: FXAA on LDR FBO → default framebuffer ----
        if (RenderSettings::instance().getFXAAEnabled())
            FXAAPass();
        else
            BlitLDRToScreen();

        entityManager.releaseMutex();
    }

    ~RenderSystem()
    {
        glDeleteFramebuffers(1, &m_depthFBO);
        glDeleteTextures(1, &m_depthTex);
        glDeleteBuffers(1, &m_lightSSBO);
        glDeleteBuffers(1, &m_lightIndexSSBO);
        glDeleteBuffers(1, &m_tileGridSSBO);
        glDeleteProgram(m_depthShader);
        glDeleteProgram(m_lightCullShader);
        glDeleteFramebuffers(1, &m_shadowFBO);
        glDeleteTextures(1, &m_shadowCubeArray);
        glDeleteBuffers(1, &m_shadowDataSSBO);
        glDeleteProgram(m_shadowShader);
        glDeleteFramebuffers(1, &m_hdrFBO);
        glDeleteTextures(1, &m_hdrColorTex);
        glDeleteTextures(1, &m_hdrDepthTex);
        glDeleteProgram(m_tonemapShader);
        glDeleteVertexArrays(1, &m_quadVAO);
        glDeleteBuffers(1, &m_quadVBO);
        // SSAO
        glDeleteFramebuffers(1, &m_ssaoFBO);
        glDeleteFramebuffers(1, &m_ssaoBlurFBO);
        glDeleteTextures(1, &m_ssaoTex);
        glDeleteTextures(1, &m_ssaoBlurTex);
        glDeleteTextures(1, &m_ssaoNoiseTex);
        glDeleteProgram(m_ssaoShader);
        glDeleteProgram(m_ssaoBlurShader);
        // Bloom
        glDeleteFramebuffers(1, &m_bloomThreshFBO);
        glDeleteFramebuffers(1, &m_bloomPingFBO);
        glDeleteFramebuffers(1, &m_bloomPongFBO);
        glDeleteTextures(1, &m_bloomThreshTex);
        glDeleteTextures(1, &m_bloomPingTex);
        glDeleteTextures(1, &m_bloomPongTex);
        glDeleteProgram(m_bloomThreshShader);
        glDeleteProgram(m_bloomKawaseShader);
        // SSR
        glDeleteFramebuffers(1, &m_ssrFBO);
        glDeleteTextures(1, &m_ssrTex);
        glDeleteProgram(m_ssrShader);
        // FXAA
        glDeleteFramebuffers(1, &m_fxaaFBO);
        glDeleteTextures(1, &m_fxaaTex);
        glDeleteProgram(m_fxaaShader);
    }

private:
    // =====================================================
    //  Init-time constants — set from RenderSettings in Init()
    // =====================================================
    int TILE_SIZE = 16;
    int MAX_LIGHTS = 512;
    int MAX_LIGHTS_TILE = 256;
    int MAX_SHADOW_LIGHTS = 8;

    // =====================================================
    //  GPU resource handles
    // =====================================================
    GLuint m_depthFBO = 0;
    GLuint m_depthTex = 0;
    GLuint m_lightSSBO = 0; // binding 0 — PointLight array
    GLuint m_lightIndexSSBO = 0; // binding 1 — flat uint index list
    GLuint m_tileGridSSBO = 0; // binding 2 — (offset, count) per tile
    GLuint m_depthShader = 0;
    GLuint m_lightCullShader = 0;

    int m_screenW = 0, m_screenH = 0;
    int m_tilesX = 0, m_tilesY = 0;
    int m_lightCount = 0;

    GLuint m_shadowCubeArray = 0;
    GLuint m_shadowFBO = 0;
    GLuint m_shadowShader = 0;
    GLuint m_shadowDataSSBO = 0;
    int    m_shadowRes = 512;
    int    m_shadowCount = 0;

    // =====================================================
    //  HDR framebuffer + screen-quad resources
    // =====================================================
    GLuint m_hdrFBO = 0;  // offscreen FBO for the shading pass
    GLuint m_hdrColorTex = 0;  // RGBA16F HDR color attachment
    GLuint m_hdrDepthTex = 0;  // shared depth attachment (same size as screen)
    GLuint m_tonemapShader = 0;  // fullscreen tonemap + gamma program
    GLuint m_quadVAO = 0;  // screen-space triangle VAO
    GLuint m_quadVBO = 0;

    // =====================================================
    //  SSAO resources
    //  Pass reads m_hdrDepthTex (view-space positions are
    //  reconstructed from depth + inverse projection).
    //  Output: R8 occlusion texture, blurred with a 2-pass
    //  separable Gaussian before compositing.
    // =====================================================
    GLuint m_ssaoFBO = 0;  // raw occlusion render target
    GLuint m_ssaoBlurFBO = 0;  // blurred occlusion render target
    GLuint m_ssaoTex = 0;  // R8  — raw SSAO output
    GLuint m_ssaoBlurTex = 0;  // R8  — blurred SSAO (used in tonemap)
    GLuint m_ssaoNoiseTex = 0;  // 4x4 RGB16F rotation noise tile
    GLuint m_ssaoShader = 0;  // SSAO generation program
    GLuint m_ssaoBlurShader = 0;  // separable Gaussian blur program

    // =====================================================
    //  Bloom resources
    //  Dual-pass Kawase filter operating entirely in HDR
    //  (RGBA16F) so the tonemap curve shapes the final glow.
    //  Two ping-pong textures are blitted alternately for
    //  m_bloomPasses downsample + upsample iterations.
    // =====================================================
    GLuint m_bloomThreshFBO = 0;  // threshold extraction FBO
    GLuint m_bloomThreshTex = 0;  // RGBA16F — bright pixels only
    GLuint m_bloomPingFBO = 0;
    GLuint m_bloomPingTex = 0;  // RGBA16F — ping buffer (half-res)
    GLuint m_bloomPongFBO = 0;
    GLuint m_bloomPongTex = 0;  // RGBA16F — pong buffer (half-res)
    GLuint m_bloomThreshShader = 0;  // brightness threshold extract
    GLuint m_bloomKawaseShader = 0;  // Kawase blur (down + up pass)

    // =====================================================
    //  SSR resources
    //  Ray-marches in screen space using the HDR depth and
    //  colour buffers already in the pipeline.  Output is an
    //  RGBA16F texture composited onto the HDR buffer before
    //  tonemapping.
    // =====================================================
    GLuint m_ssrFBO = 0;  // SSR output render target
    GLuint m_ssrTex = 0;  // RGBA16F — reflection colour
    GLuint m_ssrShader = 0;

    // =====================================================
    //  FXAA resources
    //  Applied on the final LDR buffer (after TonemapPass).
    //  Writes back to the default framebuffer via a separate
    //  intermediate RGBA8 FBO so we can sample the tonemapped
    //  result as a texture.
    // =====================================================
    GLuint m_fxaaFBO = 0;  // LDR intermediate (RGBA8)
    GLuint m_fxaaTex = 0;  // RGBA8 — tonemapped LDR
    GLuint m_fxaaShader = 0;

    // =====================================================
    //  Initialisation helpers
    // =====================================================
    void InitDepthFBO()
    {
        glGenTextures(1, &m_depthTex);
        glBindTexture(GL_TEXTURE_2D, m_depthTex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT32F,
            m_screenW, m_screenH, 0,
            GL_DEPTH_COMPONENT, GL_FLOAT, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

        glGenFramebuffers(1, &m_depthFBO);
        glBindFramebuffer(GL_FRAMEBUFFER, m_depthFBO);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
            GL_TEXTURE_2D, m_depthTex, 0);
        glDrawBuffer(GL_NONE);
        glReadBuffer(GL_NONE);

        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
            Debug::Error("RenderSystem") << "Depth FBO incomplete\n";

        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }

    void InitSSBOs()
    {
        int totalTiles = m_tilesX * m_tilesY;

        glGenBuffers(1, &m_lightSSBO);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, m_lightSSBO);
        glBufferData(GL_SHADER_STORAGE_BUFFER,
            sizeof(GPUPointLight) * MAX_LIGHTS,
            nullptr, GL_DYNAMIC_DRAW);

        glGenBuffers(1, &m_lightIndexSSBO);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, m_lightIndexSSBO);
        glBufferData(GL_SHADER_STORAGE_BUFFER,
            sizeof(uint32_t) * totalTiles * MAX_LIGHTS_TILE,
            nullptr, GL_DYNAMIC_DRAW);

        glGenBuffers(1, &m_tileGridSSBO);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, m_tileGridSSBO);
        glBufferData(GL_SHADER_STORAGE_BUFFER,
            sizeof(GPUTileData) * totalTiles,
            nullptr, GL_DYNAMIC_DRAW);

        glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
    }

    void InitShadowCubeArray()
    {
        // Read shadow resolution from RenderSettings
        m_shadowRes = RenderSettings::instance().getShadowResolution();

        glGenTextures(1, &m_shadowCubeArray);
        glBindTexture(GL_TEXTURE_CUBE_MAP_ARRAY, m_shadowCubeArray);
        glTexImage3D(GL_TEXTURE_CUBE_MAP_ARRAY, 0, GL_DEPTH_COMPONENT32F,
            m_shadowRes, m_shadowRes,
            MAX_SHADOW_LIGHTS * 6,
            0, GL_DEPTH_COMPONENT, GL_FLOAT, nullptr);

        glTexParameteri(GL_TEXTURE_CUBE_MAP_ARRAY, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_CUBE_MAP_ARRAY, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_CUBE_MAP_ARRAY, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_CUBE_MAP_ARRAY, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_CUBE_MAP_ARRAY, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);

        glGenFramebuffers(1, &m_shadowFBO);
        glBindFramebuffer(GL_FRAMEBUFFER, m_shadowFBO);
        glFramebufferTexture(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, m_shadowCubeArray, 0);
        glDrawBuffer(GL_NONE);
        glReadBuffer(GL_NONE);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);

        glGenBuffers(1, &m_shadowDataSSBO);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, m_shadowDataSSBO);
        glBufferData(GL_SHADER_STORAGE_BUFFER,
            sizeof(GPUShadowData) * MAX_SHADOW_LIGHTS,
            nullptr, GL_DYNAMIC_DRAW);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
    }

    // =====================================================
    //  Shader compilation
    // =====================================================
    static GLuint CompileStage(GLenum type, const char* src)
    {
        GLuint s = glCreateShader(type);
        glShaderSource(s, 1, &src, nullptr);
        glCompileShader(s);
        GLint ok = 0;
        glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
        if (!ok) {
            char log[512];
            glGetShaderInfoLog(s, 512, nullptr, log);
            Debug::Error("RenderSystem::Shader") << log << "\n";
        }
        return s;
    }

    static GLuint LinkProgram(std::initializer_list<GLuint> stages)
    {
        GLuint prog = glCreateProgram();
        for (GLuint s : stages) glAttachShader(prog, s);
        glLinkProgram(prog);
        GLint ok = 0;
        glGetProgramiv(prog, GL_LINK_STATUS, &ok);
        if (!ok) {
            char log[512];
            glGetProgramInfoLog(prog, 512, nullptr, log);
            Debug::Error("RenderSystem::Program") << log << "\n";
        }
        for (GLuint s : stages) { glDetachShader(prog, s); glDeleteShader(s); }
        return prog;
    }

    void CompileDepthShader()
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

    void CompileLightCullShader()
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

        m_lightCullShader = LinkProgram({
            CompileStage(GL_COMPUTE_SHADER, comp)
            });
    }

    void CompileShadowShader()
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
    //  Per-frame passes
    // =====================================================

    void UploadLights(EntityManager& em)
    {
        std::vector<GPUPointLight> lights;
        lights.reserve(MAX_LIGHTS);

        auto q = em.CreateQuery<PointLightComponent, Transform>();
        for (auto [entity, light, xform] : q) {
            if ((int)lights.size() >= MAX_LIGHTS) break;
            GPUPointLight gl;
            gl.posRadius = glm::vec4(xform->getPosition(), light->radius);
            gl.colorIntensity = glm::vec4(light->color, light->intensity);
            lights.push_back(gl);
        }

        m_lightCount = (int)lights.size();

        glBindBuffer(GL_SHADER_STORAGE_BUFFER, m_lightSSBO);
        glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0,
            sizeof(GPUPointLight) * m_lightCount, lights.data());
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
    }

    void ShadowPass(EntityManager& em, auto& meshQuery)
    {
        const auto& rs = RenderSettings::instance();

        std::vector<GPUShadowData> shadowDataVec;

        glBindFramebuffer(GL_FRAMEBUFFER, m_shadowFBO);
        glViewport(0, 0, m_shadowRes, m_shadowRes);
        glClear(GL_DEPTH_BUFFER_BIT);
        glEnable(GL_DEPTH_TEST);
        glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
        glEnable(GL_POLYGON_OFFSET_FILL);
        glPolygonOffset(rs.getShadowBiasFactor(), rs.getShadowBiasUnits()); // runtime setting

        glUseProgram(m_shadowShader);

        int  shadowIdx = 0;
        auto lightQuery = em.CreateQuery<PointLightComponent, Transform>();

        for (auto [entity, light, xform] : lightQuery)
        {
            if (shadowIdx >= MAX_SHADOW_LIGHTS) break;

            glm::vec3 pos = xform->getPosition();
            float     farPlane = light->radius;

            glm::mat4 shadowProj = glm::perspective(glm::radians(90.0f), 1.0f,
                rs.getShadowNearPlane(), farPlane);

            glm::mat4 views[6] = {
                shadowProj * glm::lookAt(pos, pos + glm::vec3(1, 0, 0), glm::vec3(0,-1, 0)),
                shadowProj * glm::lookAt(pos, pos + glm::vec3(-1, 0, 0), glm::vec3(0,-1, 0)),
                shadowProj * glm::lookAt(pos, pos + glm::vec3(0, 1, 0), glm::vec3(0, 0, 1)),
                shadowProj * glm::lookAt(pos, pos + glm::vec3(0,-1, 0), glm::vec3(0, 0,-1)),
                shadowProj * glm::lookAt(pos, pos + glm::vec3(0, 0, 1), glm::vec3(0,-1, 0)),
                shadowProj * glm::lookAt(pos, pos + glm::vec3(0, 0,-1), glm::vec3(0,-1, 0)),
            };

            for (int f = 0; f < 6; f++) {
                std::string uni = "uLightSpaceMatrices[" + std::to_string(f) + "]";
                glUniformMatrix4fv(glGetUniformLocation(m_shadowShader, uni.c_str()),
                    1, GL_FALSE, glm::value_ptr(views[f]));
            }
            glUniform3fv(glGetUniformLocation(m_shadowShader, "uLightPos"),
                1, glm::value_ptr(pos));
            glUniform1f(glGetUniformLocation(m_shadowShader, "uFarPlane"), farPlane);
            glUniform1i(glGetUniformLocation(m_shadowShader, "uCubeArrayLayer"), shadowIdx * 6);

            for (auto [me, meshC, xf] : meshQuery) {
                if (!meshC->enabled || !meshC->mesh) continue;
                glUniformMatrix4fv(glGetUniformLocation(m_shadowShader, "uModel"),
                    1, GL_FALSE, glm::value_ptr(xf->getModelMatrix()));
                meshC->mesh->drawDepthOnly(glm::mat4(1.0f), m_shadowShader);
            }

            GPUShadowData sd;
            for (int f = 0; f < 6; f++) sd.lightSpaceMatrices[f] = views[f];
            sd.lightIndex = shadowIdx;
            sd.farPlane = farPlane;
            shadowDataVec.push_back(sd);
            shadowIdx++;
        }

        m_shadowCount = shadowIdx;

        glBindBuffer(GL_SHADER_STORAGE_BUFFER, m_shadowDataSSBO);
        glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0,
            sizeof(GPUShadowData) * m_shadowCount, shadowDataVec.data());
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);

        glDisable(GL_POLYGON_OFFSET_FILL);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glViewport(0, 0, m_screenW, m_screenH);
    }

    void DepthPrePass(auto& meshQuery,
        const glm::mat4& view,
        const glm::mat4& projection)
    {
        glBindFramebuffer(GL_FRAMEBUFFER, m_depthFBO);
        glClear(GL_DEPTH_BUFFER_BIT);
        glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
        glEnable(GL_DEPTH_TEST);

        glUseProgram(m_depthShader);

        for (auto [entity, meshC, transform] : meshQuery) {
            if (!meshC->enabled) continue;
            Mesh* mesh = meshC->mesh.get();
            if (!mesh) continue;

            glm::mat4 mvp = projection * view * transform->getModelMatrix();
            mesh->drawDepthOnly(mvp, m_depthShader);
        }

        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }

    void LightCullPass(const glm::mat4& view, const glm::mat4& projection)
    {
        glUseProgram(m_lightCullShader);

        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, m_depthTex);
        glUniform1i(glGetUniformLocation(m_lightCullShader, "uDepthMap"), 0);

        glUniformMatrix4fv(glGetUniformLocation(m_lightCullShader, "uProjection"),
            1, GL_FALSE, glm::value_ptr(projection));
        glUniformMatrix4fv(glGetUniformLocation(m_lightCullShader, "uView"),
            1, GL_FALSE, glm::value_ptr(view));
        glUniform1i(glGetUniformLocation(m_lightCullShader, "uLightCount"), m_lightCount);
        glUniform2i(glGetUniformLocation(m_lightCullShader, "uScreenSize"), m_screenW, m_screenH);

        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, m_lightSSBO);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, m_lightIndexSSBO);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, m_tileGridSSBO);

        glDispatchCompute((GLuint)m_tilesX, (GLuint)m_tilesY, 1);
        glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);
    }

    void ShadingPass(auto& meshQuery, const glm::mat4& view,
        const glm::mat4& projection, const glm::vec3& cameraPos)
    {
        // Render into the floating-point HDR framebuffer so that
        // scene luminance is preserved before tonemapping.
        glBindFramebuffer(GL_FRAMEBUFFER, m_hdrFBO);
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glEnable(GL_DEPTH_TEST);

        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, m_lightSSBO);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, m_lightIndexSSBO);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, m_tileGridSSBO);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 3, m_shadowDataSSBO);

        glActiveTexture(GL_TEXTURE5);
        glBindTexture(GL_TEXTURE_CUBE_MAP_ARRAY, m_shadowCubeArray);

        for (auto [entity, meshC, transform] : meshQuery) {
            if (!meshC->enabled || !meshC->mesh) continue;

            glm::mat4 model = transform->getModelMatrix();
            meshC->mesh->bindMaterial(model, view, projection);

            if (Material* mat = meshC->mesh->getMaterial()) {
                mat->setIVec2("uScreenSize", glm::ivec2(m_screenW, m_screenH));
                mat->setVec3("uCameraPos", cameraPos);
                mat->setInt("uShadowCubeArray", 5);
                mat->setInt("uShadowCount", m_shadowCount);
            }
            meshC->mesh->draw();
        }

        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }

    // =====================================================
    //  HDR framebuffer initialisation
    //  Creates an RGBA16F colour attachment and a shared
    //  depth attachment at the current screen resolution.
    // =====================================================
    void InitHDRFBO()
    {
        // --- Floating-point colour texture ---
        glGenTextures(1, &m_hdrColorTex);
        glBindTexture(GL_TEXTURE_2D, m_hdrColorTex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F,
            m_screenW, m_screenH, 0,
            GL_RGBA, GL_FLOAT, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

        // --- Depth texture (reused from the scene depth; recreated here
        //     so the HDR FBO has its own depth buffer) ---
        glGenTextures(1, &m_hdrDepthTex);
        glBindTexture(GL_TEXTURE_2D, m_hdrDepthTex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT32F,
            m_screenW, m_screenH, 0,
            GL_DEPTH_COMPONENT, GL_FLOAT, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

        // --- Framebuffer ---
        glGenFramebuffers(1, &m_hdrFBO);
        glBindFramebuffer(GL_FRAMEBUFFER, m_hdrFBO);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
            GL_TEXTURE_2D, m_hdrColorTex, 0);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
            GL_TEXTURE_2D, m_hdrDepthTex, 0);

        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
            Debug::Error("RenderSystem") << "HDR FBO incomplete\n";

        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glBindTexture(GL_TEXTURE_2D, 0);
    }

    // =====================================================
    //  Screen-space triangle (covers the full NDC clip)
    // =====================================================
    void InitScreenQuad()
    {
        // A single large triangle that covers the entire screen.
        // No index buffer needed; clip-space coords go past [-1,1]
        // but the rasteriser clips them for us.
        static const float kVerts[] = {
            // position (NDC)   texcoord
            -1.0f, -1.0f,       0.0f, 0.0f,
             3.0f, -1.0f,       2.0f, 0.0f,
            -1.0f,  3.0f,       0.0f, 2.0f,
        };

        glGenVertexArrays(1, &m_quadVAO);
        glGenBuffers(1, &m_quadVBO);

        glBindVertexArray(m_quadVAO);
        glBindBuffer(GL_ARRAY_BUFFER, m_quadVBO);
        glBufferData(GL_ARRAY_BUFFER, sizeof(kVerts), kVerts, GL_STATIC_DRAW);

        // location 0 — vec2 position
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE,
            4 * sizeof(float), (void*)0);

        // location 1 — vec2 texcoord
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE,
            4 * sizeof(float), (void*)(2 * sizeof(float)));

        glBindVertexArray(0);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
    }

    // =====================================================
    //  Tonemap post-process shader
    //
    //  Implements two operators, selected via uFilmicEnabled:
    //
    //    Reinhard:  y = x / (1 + x)
    //               Simple, always finite, no parameters.
    //
    //    Filmic:    Uncharted 2 / John Hable curve.
    //               f(x) = ((x*(A*x+C*B)+D*E)/(x*(A*x+B)+D*F)) - E/F
    //               where A..F map to the RenderSettings curve params.
    //               Result is normalised by f(W) so white maps to 1.
    //
    //  Gamma correction (pow(col, 1/gamma)) is applied after both.
    // =====================================================
    void CompileTonemapShader()
    {
        const char* vert = R"GLSL(
            #version 430 core
            layout(location = 0) in vec2 aPos;
            layout(location = 1) in vec2 aUV;
            out vec2 vUV;
            void main()
            {
                vUV         = aUV;
                gl_Position = vec4(aPos, 0.0, 1.0);
            }
        )GLSL";

        const char* frag = R"GLSL(
            #version 430 core
            in  vec2 vUV;
            out vec4 FragColor;

            uniform sampler2D uHDRBuffer;
            uniform sampler2D uSSAOTex;     // blurred R8 occlusion (unit 1)
            uniform sampler2D uBloomTex;    // half-res HDR bloom    (unit 2)
            uniform sampler2D uSSRTex;      // full-res HDR reflections (unit 3)

            // Exposure and mode
            uniform float uExposure;
            uniform bool  uFilmicEnabled;
            uniform float uGamma;

            // Feature toggles
            uniform bool  uSSAOEnabled;
            uniform bool  uBloomEnabled;
            uniform float uBloomStrength;
            uniform bool  uSSREnabled;

            // Filmic (Uncharted 2 / Hable) curve parameters
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

                // --- SSR composite (additive, before tonemap) ---
                // The SSR texture encodes reflections pre-weighted by
                // metallic*(1-roughness), so we add directly.
                if (uSSREnabled)
                    hdrColor += texture(uSSRTex, vUV).rgb;

                // --- Ambient occlusion ---
                if (uSSAOEnabled)
                {
                    float ao = texture(uSSAOTex, vUV).r;
                    hdrColor += vec3(0.03) * ao;
                }
                else
                {
                    hdrColor += vec3(0.03);
                }

                // --- Bloom composite (additive, before tonemap) ---
                if (uBloomEnabled)
                    hdrColor += texture(uBloomTex, vUV).rgb * uBloomStrength;

                // --- Exposure ---
                hdrColor *= uExposure;

                // --- Tonemap ---
                vec3 mapped;
                if (uFilmicEnabled)
                {
                    vec3 whiteScale = vec3(1.0) / FilmicCurve(vec3(uW));
                    mapped = FilmicCurve(hdrColor) * whiteScale;
                }
                else
                {
                    mapped = hdrColor / (hdrColor + vec3(1.0));
                }

                // --- Gamma correction ---
                mapped = pow(clamp(mapped, 0.0, 1.0), vec3(1.0 / uGamma));

                FragColor = vec4(mapped, 1.0);
            }
        )GLSL";

        m_tonemapShader = LinkProgram({
            CompileStage(GL_VERTEX_SHADER,   vert),
            CompileStage(GL_FRAGMENT_SHADER, frag)
            });
    }

    // =====================================================
    //  TonemapPass
    //  Reads HDR buffer, composites SSAO + SSR + Bloom,
    //  applies exposure + tonemapping + gamma.
    //  When FXAA is enabled writes to m_fxaaFBO (LDR RGBA8)
    //  so FXAAPass can sample it; otherwise writes directly
    //  to the default framebuffer.
    // =====================================================
    void TonemapPass()
    {
        const auto& rs = RenderSettings::instance();
        bool fxaaOn = rs.getFXAAEnabled();

        // Target: intermediate LDR FBO if FXAA will follow,
        // otherwise write straight to the default framebuffer.
        glBindFramebuffer(GL_FRAMEBUFFER, fxaaOn ? m_fxaaFBO : 0);
        glViewport(0, 0, m_screenW, m_screenH);
        glDisable(GL_DEPTH_TEST);
        glClear(GL_COLOR_BUFFER_BIT);

        glUseProgram(m_tonemapShader);

        // Unit 0 — HDR colour buffer
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, m_hdrColorTex);
        glUniform1i(glGetUniformLocation(m_tonemapShader, "uHDRBuffer"), 0);

        // Unit 1 — blurred SSAO occlusion (R8)
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, rs.getSSAOEnabled() ? m_ssaoBlurTex : 0);
        glUniform1i(glGetUniformLocation(m_tonemapShader, "uSSAOTex"), 1);

        // Unit 2 — bloom result (RGBA16F half-res)
        glActiveTexture(GL_TEXTURE2);
        glBindTexture(GL_TEXTURE_2D,
            (rs.getBloomEnabled() && m_bloomResultTex) ? m_bloomResultTex : 0);
        glUniform1i(glGetUniformLocation(m_tonemapShader, "uBloomTex"), 2);

        // Unit 3 — SSR reflection colour (RGBA16F full-res)
        glActiveTexture(GL_TEXTURE3);
        glBindTexture(GL_TEXTURE_2D, rs.getSSREnabled() ? m_ssrTex : 0);
        glUniform1i(glGetUniformLocation(m_tonemapShader, "uSSRTex"), 3);

        // Feature flags
        glUniform1i(glGetUniformLocation(m_tonemapShader, "uSSAOEnabled"),
            rs.getSSAOEnabled() ? 1 : 0);
        glUniform1i(glGetUniformLocation(m_tonemapShader, "uBloomEnabled"),
            rs.getBloomEnabled() ? 1 : 0);
        glUniform1f(glGetUniformLocation(m_tonemapShader, "uBloomStrength"),
            rs.getBloomStrength());
        glUniform1i(glGetUniformLocation(m_tonemapShader, "uSSREnabled"),
            rs.getSSREnabled() ? 1 : 0);

        // Exposure / mode
        glUniform1f(glGetUniformLocation(m_tonemapShader, "uExposure"), rs.getExposure());
        glUniform1i(glGetUniformLocation(m_tonemapShader, "uFilmicEnabled"), rs.getFilmicEnabled() ? 1 : 0);
        glUniform1f(glGetUniformLocation(m_tonemapShader, "uGamma"), rs.getGamma());

        // Filmic curve params
        glUniform1f(glGetUniformLocation(m_tonemapShader, "uA"), rs.getFilmicShoulder());
        glUniform1f(glGetUniformLocation(m_tonemapShader, "uB"), rs.getFilmicLinearStrength());
        glUniform1f(glGetUniformLocation(m_tonemapShader, "uC"), rs.getFilmicLinearAngle());
        glUniform1f(glGetUniformLocation(m_tonemapShader, "uD"), rs.getFilmicToeStrength());
        glUniform1f(glGetUniformLocation(m_tonemapShader, "uE"), rs.getFilmicToeNumerator());
        glUniform1f(glGetUniformLocation(m_tonemapShader, "uF"), rs.getFilmicToeDenominator());
        glUniform1f(glGetUniformLocation(m_tonemapShader, "uW"), rs.getFilmicLinearWhite());

        glBindVertexArray(m_quadVAO);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        glBindVertexArray(0);

        glEnable(GL_DEPTH_TEST);
    }

    // Simple blit when FXAA is off — copies m_fxaaFBO to screen.
    // Not called when FXAA is on (FXAAPass does the final write).
    // When FXAA is disabled TonemapPass already wrote to FB 0,
    // so this function is only here for completeness and won't
    // actually execute (the call site checks getFXAAEnabled()).
    void BlitLDRToScreen() { /* TonemapPass already wrote to FB 0 */ }
    // =====================================================
    //  SSAO — initialise FBOs and noise texture
    //  Screen-size FBOs hold R8 occlusion maps (full-res).
    //  The 4x4 noise tile is tiled across the screen to
    //  randomise the sample kernel orientation per-pixel,
    //  breaking up the banding that appears with a fixed
    //  hemisphere orientation.
    // =====================================================
    void InitSSAO()
    {
        // --- Raw SSAO FBO (R8, full resolution) ---
        glGenTextures(1, &m_ssaoTex);
        glBindTexture(GL_TEXTURE_2D, m_ssaoTex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, m_screenW, m_screenH,
            0, GL_RED, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

        glGenFramebuffers(1, &m_ssaoFBO);
        glBindFramebuffer(GL_FRAMEBUFFER, m_ssaoFBO);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
            GL_TEXTURE_2D, m_ssaoTex, 0);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
            Debug::Error("RenderSystem") << "SSAO FBO incomplete\n";

        // --- Blur FBO (R8, full resolution) ---
        glGenTextures(1, &m_ssaoBlurTex);
        glBindTexture(GL_TEXTURE_2D, m_ssaoBlurTex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, m_screenW, m_screenH,
            0, GL_RED, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

        glGenFramebuffers(1, &m_ssaoBlurFBO);
        glBindFramebuffer(GL_FRAMEBUFFER, m_ssaoBlurFBO);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
            GL_TEXTURE_2D, m_ssaoBlurTex, 0);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
            Debug::Error("RenderSystem") << "SSAO blur FBO incomplete\n";

        glBindFramebuffer(GL_FRAMEBUFFER, 0);

        // --- 4x4 noise texture (already generated; skip if exists) ---
        if (m_ssaoNoiseTex != 0) return;

        // 16 random vectors in the XY plane — used to rotate the
        // sample hemisphere per pixel, eliminating regular banding.
        std::vector<glm::vec3> ssaoNoise;
        ssaoNoise.reserve(16);
        // Use a fixed seed for reproducibility; change if desired.
        srand(42);
        for (int i = 0; i < 16; ++i)
        {
            glm::vec3 noise(
                (float)rand() / RAND_MAX * 2.0f - 1.0f,
                (float)rand() / RAND_MAX * 2.0f - 1.0f,
                0.0f);
            ssaoNoise.push_back(noise);
        }

        glGenTextures(1, &m_ssaoNoiseTex);
        glBindTexture(GL_TEXTURE_2D, m_ssaoNoiseTex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB16F, 4, 4, 0,
            GL_RGB, GL_FLOAT, ssaoNoise.data());
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        // REPEAT so the 4x4 tile wraps across the full screen
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
        glBindTexture(GL_TEXTURE_2D, 0);
    }

    // =====================================================
    //  Bloom — initialise threshold + ping-pong FBOs
    //  All textures are RGBA16F (half-res) so the Kawase
    //  blur operates in HDR and preserves colour fidelity.
    // =====================================================
    void InitBloom()
    {
        int bW = std::max(1, m_screenW / 2);
        int bH = std::max(1, m_screenH / 2);

        auto makeBloomTex = [&](GLuint& tex, GLuint& fbo, int w, int h)
            {
                glGenTextures(1, &tex);
                glBindTexture(GL_TEXTURE_2D, tex);
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, w, h,
                    0, GL_RGBA, GL_FLOAT, nullptr);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

                glGenFramebuffers(1, &fbo);
                glBindFramebuffer(GL_FRAMEBUFFER, fbo);
                glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                    GL_TEXTURE_2D, tex, 0);
                if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
                    Debug::Error("RenderSystem") << "Bloom FBO incomplete\n";
            };

        // Threshold FBO — full-res extract of bright pixels
        makeBloomTex(m_bloomThreshTex, m_bloomThreshFBO, m_screenW, m_screenH);
        // Ping-pong buffers — half-res for the Kawase blur iterations
        makeBloomTex(m_bloomPingTex, m_bloomPingFBO, bW, bH);
        makeBloomTex(m_bloomPongTex, m_bloomPongFBO, bW, bH);

        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glBindTexture(GL_TEXTURE_2D, 0);
    }

    // =====================================================
    //  SSAO shaders
    //
    //  ssaoShader:
    //    Reconstructs view-space position from depth and the
    //    inverse projection matrix, samples a hemisphere of
    //    random offsets (rotated by the noise texture), and
    //    counts how many samples are occluded.
    //
    //  ssaoBlurShader:
    //    Simple 4x4 box blur on the raw SSAO output to
    //    smooth the noise-tiled hemisphere pattern.
    // =====================================================
    void CompileSSAOShaders()
    {
        // Shared fullscreen vertex shader (same as tonemap)
        const char* quadVert = R"GLSL(
            #version 430 core
            layout(location = 0) in vec2 aPos;
            layout(location = 1) in vec2 aUV;
            out vec2 vUV;
            void main() { vUV = aUV; gl_Position = vec4(aPos, 0.0, 1.0); }
        )GLSL";

        // --------------------------------------------------
        //  SSAO generation fragment shader
        //
        //  Inputs:
        //    uDepthTex    — GL_DEPTH_COMPONENT32F from HDR FBO
        //    uNoiseTex    — 4x4 RGB16F rotation noise tile
        //    uProjection  — camera projection matrix
        //    uInvProj     — inverse projection
        //    uSamples[]   — 64 hemisphere sample offsets (view-space)
        //    uKernelSize  — active sample count (from RenderSettings)
        //    uRadius      — hemisphere radius (view-space units)
        //    uBias        — depth bias to prevent self-occlusion
        //    uPower       — power curve on occlusion factor
        //    uScreenSize  — pixel dimensions (for noise tiling scale)
        // --------------------------------------------------
        const char* ssaoFrag = R"GLSL(
            #version 430 core
            in  vec2 vUV;
            out float FragOcclusion;

            uniform sampler2D uDepthTex;
            uniform sampler2D uNoiseTex;
            uniform mat4      uProjection;
            uniform mat4      uInvProj;
            uniform vec3      uSamples[64];
            uniform int       uKernelSize;
            uniform float     uRadius;
            uniform float     uBias;
            uniform float     uPower;
            uniform ivec2     uScreenSize;

            // Reconstruct view-space position from a UV + depth sample
            vec3 ReconstructViewPos(vec2 uv, float depth)
            {
                // Convert depth from [0,1] to NDC z in [-1,1]
                vec4 ndcPos = vec4(uv * 2.0 - 1.0, depth * 2.0 - 1.0, 1.0);
                vec4 viewPos = uInvProj * ndcPos;
                return viewPos.xyz / viewPos.w;
            }

            void main()
            {
                // Reconstruct view-space position and normal at this fragment
                float depth      = texture(uDepthTex, vUV).r;

                // Skip sky / far-plane fragments (depth == 1.0)
                if (depth >= 0.9999) { FragOcclusion = 1.0; return; }

                vec3  fragPos    = ReconstructViewPos(vUV, depth);

                // Approximate view-space normal from depth derivatives
                vec3  dPdx   = dFdx(fragPos);
                vec3  dPdy   = dFdy(fragPos);
                vec3  normal = normalize(cross(dPdx, dPdy));
                // dFdx/dFdy give the correct outward normal in view space
                // as long as the depth buffer was written front-to-back.

                // Tile the noise texture across the screen (4x4 tile)
                vec2  noiseScale = vec2(uScreenSize) / 4.0;
                vec3  randomVec  = normalize(texture(uNoiseTex, vUV * noiseScale).xyz);

                // Build TBN to rotate sample kernel into view-space hemisphere
                vec3 tangent   = normalize(randomVec - normal * dot(randomVec, normal));
                vec3 bitangent = cross(normal, tangent);
                mat3 TBN       = mat3(tangent, bitangent, normal);

                float occlusion = 0.0;
                for (int i = 0; i < uKernelSize; ++i)
                {
                    // Transform sample from tangent to view space
                    vec3 samplePos = TBN * uSamples[i];
                    samplePos = fragPos + samplePos * uRadius;

                    // Project sample to get its UV + depth
                    vec4 offset = uProjection * vec4(samplePos, 1.0);
                    offset.xyz /= offset.w;
                    offset.xyz  = offset.xyz * 0.5 + 0.5; // to [0,1]

                    float sampleDepth = texture(uDepthTex, offset.xy).r;
                    vec3  sampleView  = ReconstructViewPos(offset.xy, sampleDepth);

                    // Range check: only occlude if the sample is within radius
                    float rangeCheck  = smoothstep(0.0, 1.0,
                        uRadius / abs(fragPos.z - sampleView.z + 1e-5));

                    // Occluded if the real geometry is closer than the sample
                    occlusion += (sampleView.z >= samplePos.z + uBias ? 1.0 : 0.0)
                                 * rangeCheck;
                }

                // Normalise and invert: 1.0 = fully lit, 0.0 = fully occluded
                occlusion = 1.0 - (occlusion / float(uKernelSize));
                FragOcclusion = pow(occlusion, uPower);
            }
        )GLSL";

        // --------------------------------------------------
        //  SSAO 4x4 box blur fragment shader
        //  Reads the raw R8 SSAO texture and averages a 4x4
        //  neighbourhood to smooth the noise pattern.
        // --------------------------------------------------
        const char* blurFrag = R"GLSL(
            #version 430 core
            in  vec2 vUV;
            out float FragOcclusion;

            uniform sampler2D uSSAOTex;

            void main()
            {
                vec2  texelSize = 1.0 / vec2(textureSize(uSSAOTex, 0));
                float result    = 0.0;
                for (int x = -2; x <= 1; ++x)
                    for (int y = -2; y <= 1; ++y)
                        result += texture(uSSAOTex,
                            vUV + vec2(float(x), float(y)) * texelSize).r;
                FragOcclusion = result / 16.0;
            }
        )GLSL";

        m_ssaoShader = LinkProgram({ CompileStage(GL_VERTEX_SHADER, quadVert),
                                         CompileStage(GL_FRAGMENT_SHADER, ssaoFrag) });
        m_ssaoBlurShader = LinkProgram({ CompileStage(GL_VERTEX_SHADER, quadVert),
                                         CompileStage(GL_FRAGMENT_SHADER, blurFrag) });

        // Pre-generate the hemisphere sample kernel once and upload it.
        // Samples are distributed in a unit hemisphere using a cosine-weighted
        // distribution with an accelerating interpolation so more samples
        // cluster near the origin (better local detail).
        glUseProgram(m_ssaoShader);
        srand(1337);
        for (int i = 0; i < 64; ++i)
        {
            glm::vec3 sample(
                (float)rand() / RAND_MAX * 2.0f - 1.0f,
                (float)rand() / RAND_MAX * 2.0f - 1.0f,
                (float)rand() / RAND_MAX          // z >= 0 → hemisphere
            );
            sample = glm::normalize(sample);
            sample *= ((float)rand() / RAND_MAX); // random length

            // Accelerating interpolation: more samples near the origin
            float scale = (float)i / 64.0f;
            scale = 0.1f + scale * scale * 0.9f;
            sample *= scale;

            std::string uni = "uSamples[" + std::to_string(i) + "]";
            glUniform3fv(glGetUniformLocation(m_ssaoShader, uni.c_str()),
                1, glm::value_ptr(sample));
        }
        glUseProgram(0);
    }

    // =====================================================
    //  Bloom shaders
    //
    //  bloomThreshShader:
    //    Extracts pixels whose luminance exceeds the threshold
    //    (read from RenderSettings every frame).
    //
    //  bloomKawaseShader:
    //    Single Kawase pass — samples 4 neighbours at a
    //    distance of (iteration + 0.5) pixels.  Applied
    //    alternately to ping and pong buffers.
    // =====================================================
    void CompileBloomShaders()
    {
        const char* quadVert = R"GLSL(
            #version 430 core
            layout(location = 0) in vec2 aPos;
            layout(location = 1) in vec2 aUV;
            out vec2 vUV;
            void main() { vUV = aUV; gl_Position = vec4(aPos, 0.0, 1.0); }
        )GLSL";

        // Brightness threshold extract
        const char* threshFrag = R"GLSL(
            #version 430 core
            in  vec2 vUV;
            out vec4 FragColor;

            uniform sampler2D uHDRBuffer;
            uniform float     uThreshold;

            void main()
            {
                vec3 color = texture(uHDRBuffer, vUV).rgb;
                // Perceptual luminance (BT.709 coefficients)
                float lum = dot(color, vec3(0.2126, 0.7152, 0.0722));
                // Soft knee: smoothly transition above the threshold
                float weight = smoothstep(uThreshold - 0.1, uThreshold + 0.1, lum);
                FragColor = vec4(color * weight, 1.0);
            }
        )GLSL";

        // Kawase blur pass
        // uIteration controls the sample offset distance.
        // Applied at half-resolution to reduce cost.
        const char* kawaseFrag = R"GLSL(
            #version 430 core
            in  vec2 vUV;
            out vec4 FragColor;

            uniform sampler2D uBloomTex;
            uniform int       uIteration;

            void main()
            {
                vec2  texelSize = 1.0 / vec2(textureSize(uBloomTex, 0));
                float offset    = float(uIteration) + 0.5;

                // Sample the four diagonal neighbours at the Kawase offset
                vec4 sum = vec4(0.0);
                sum += texture(uBloomTex, vUV + vec2(-offset, -offset) * texelSize);
                sum += texture(uBloomTex, vUV + vec2( offset, -offset) * texelSize);
                sum += texture(uBloomTex, vUV + vec2(-offset,  offset) * texelSize);
                sum += texture(uBloomTex, vUV + vec2( offset,  offset) * texelSize);
                FragColor = sum * 0.25;
            }
        )GLSL";

        m_bloomThreshShader = LinkProgram({ CompileStage(GL_VERTEX_SHADER, quadVert),
                                            CompileStage(GL_FRAGMENT_SHADER, threshFrag) });
        m_bloomKawaseShader = LinkProgram({ CompileStage(GL_VERTEX_SHADER, quadVert),
                                            CompileStage(GL_FRAGMENT_SHADER, kawaseFrag) });
    }

    // =====================================================
    //  SSAOPass
    //  1. Render raw occlusion into m_ssaoFBO
    //  2. Box-blur the result into m_ssaoBlurFBO
    //  TonemapPass will read m_ssaoBlurTex and multiply
    //  the ambient contribution per pixel.
    // =====================================================
    void SSAOPass(const glm::mat4& projection)
    {
        const auto& rs = RenderSettings::instance();

        glm::mat4 invProj = glm::inverse(projection);

        // ---- Step 1: generate raw SSAO ----
        glBindFramebuffer(GL_FRAMEBUFFER, m_ssaoFBO);
        glViewport(0, 0, m_screenW, m_screenH);
        glDisable(GL_DEPTH_TEST);
        glClear(GL_COLOR_BUFFER_BIT);

        glUseProgram(m_ssaoShader);

        // Depth texture from the HDR FBO (written by ShadingPass)
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, m_hdrDepthTex);
        glUniform1i(glGetUniformLocation(m_ssaoShader, "uDepthTex"), 0);

        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, m_ssaoNoiseTex);
        glUniform1i(glGetUniformLocation(m_ssaoShader, "uNoiseTex"), 1);

        glUniformMatrix4fv(glGetUniformLocation(m_ssaoShader, "uProjection"),
            1, GL_FALSE, glm::value_ptr(projection));
        glUniformMatrix4fv(glGetUniformLocation(m_ssaoShader, "uInvProj"),
            1, GL_FALSE, glm::value_ptr(invProj));
        glUniform1i(glGetUniformLocation(m_ssaoShader, "uKernelSize"), rs.getSSAOSamples());
        glUniform1f(glGetUniformLocation(m_ssaoShader, "uRadius"), rs.getSSAORadius());
        glUniform1f(glGetUniformLocation(m_ssaoShader, "uBias"), rs.getSSAOBias());
        glUniform1f(glGetUniformLocation(m_ssaoShader, "uPower"), rs.getSSAOPower());
        glUniform2i(glGetUniformLocation(m_ssaoShader, "uScreenSize"), m_screenW, m_screenH);

        glBindVertexArray(m_quadVAO);
        glDrawArrays(GL_TRIANGLES, 0, 3);

        // ---- Step 2: blur SSAO ----
        glBindFramebuffer(GL_FRAMEBUFFER, m_ssaoBlurFBO);
        glClear(GL_COLOR_BUFFER_BIT);

        glUseProgram(m_ssaoBlurShader);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, m_ssaoTex);
        glUniform1i(glGetUniformLocation(m_ssaoBlurShader, "uSSAOTex"), 0);

        glDrawArrays(GL_TRIANGLES, 0, 3);

        glBindVertexArray(0);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glEnable(GL_DEPTH_TEST);
    }

    // =====================================================
    //  BloomPass
    //  1. Extract bright pixels (threshold) at full-res
    //  2. Downsample to half-res ping buffer
    //  3. Kawase blur: ping→pong→ping... for N passes
    //  Result lives in whichever ping-pong buffer is last.
    //  TonemapPass reads it and additively blends it.
    // =====================================================
    void BloomPass()
    {
        const auto& rs = RenderSettings::instance();
        int   passes = glm::clamp(rs.getBloomPasses(), 1, 8);
        float threshold = rs.getBloomThreshold();
        int   bW = std::max(1, m_screenW / 2);
        int   bH = std::max(1, m_screenH / 2);

        glDisable(GL_DEPTH_TEST);

        // ---- Step 1: threshold extract (full-res → m_bloomThreshFBO) ----
        glBindFramebuffer(GL_FRAMEBUFFER, m_bloomThreshFBO);
        glViewport(0, 0, m_screenW, m_screenH);
        glClear(GL_COLOR_BUFFER_BIT);

        glUseProgram(m_bloomThreshShader);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, m_hdrColorTex);
        glUniform1i(glGetUniformLocation(m_bloomThreshShader, "uHDRBuffer"), 0);
        glUniform1f(glGetUniformLocation(m_bloomThreshShader, "uThreshold"), threshold);

        glBindVertexArray(m_quadVAO);
        glDrawArrays(GL_TRIANGLES, 0, 3);

        // ---- Step 2: initial downsample (full-res → half-res ping) ----
        glBindFramebuffer(GL_FRAMEBUFFER, m_bloomPingFBO);
        glViewport(0, 0, bW, bH);
        glClear(GL_COLOR_BUFFER_BIT);

        // Reuse the Kawase shader at iteration 0 as a simple downsample
        glUseProgram(m_bloomKawaseShader);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, m_bloomThreshTex);
        glUniform1i(glGetUniformLocation(m_bloomKawaseShader, "uBloomTex"), 0);
        glUniform1i(glGetUniformLocation(m_bloomKawaseShader, "uIteration"), 0);
        glDrawArrays(GL_TRIANGLES, 0, 3);

        // ---- Step 3: Kawase ping-pong blur ----
        // Alternate between ping and pong, incrementing the iteration
        // each pass to widen the filter kernel progressively.
        GLuint src = m_bloomPingTex;
        GLuint dstFBO = m_bloomPongFBO;

        for (int i = 1; i < passes; ++i)
        {
            glBindFramebuffer(GL_FRAMEBUFFER, dstFBO);
            glClear(GL_COLOR_BUFFER_BIT);

            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, src);
            glUniform1i(glGetUniformLocation(m_bloomKawaseShader, "uBloomTex"), 0);
            glUniform1i(glGetUniformLocation(m_bloomKawaseShader, "uIteration"), i);
            glDrawArrays(GL_TRIANGLES, 0, 3);

            // Swap ping/pong
            if (dstFBO == m_bloomPongFBO) { dstFBO = m_bloomPingFBO; src = m_bloomPongTex; }
            else { dstFBO = m_bloomPongFBO; src = m_bloomPingTex; }
        }

        // After the loop, 'src' holds the last written texture.
        // Store which texture has the final bloom result so TonemapPass
        // can bind it regardless of the pass count parity.
        m_bloomResultTex = src;

        glBindVertexArray(0);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glViewport(0, 0, m_screenW, m_screenH);
        glEnable(GL_DEPTH_TEST);
    }

    GLuint m_bloomResultTex = 0; // points to ping or pong after BloomPass

    // =====================================================
    //  SSR — init full-res RGBA16F render target
    // =====================================================
    void InitSSR()
    {
        glGenTextures(1, &m_ssrTex);
        glBindTexture(GL_TEXTURE_2D, m_ssrTex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F,
            m_screenW, m_screenH, 0, GL_RGBA, GL_FLOAT, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

        glGenFramebuffers(1, &m_ssrFBO);
        glBindFramebuffer(GL_FRAMEBUFFER, m_ssrFBO);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
            GL_TEXTURE_2D, m_ssrTex, 0);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
            Debug::Error("RenderSystem") << "SSR FBO incomplete\n";

        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glBindTexture(GL_TEXTURE_2D, 0);
    }

    // =====================================================
    //  FXAA — init LDR intermediate RGBA8 render target
    //  TonemapPass writes here; FXAAPass samples it and
    //  writes the anti-aliased result to FB 0.
    // =====================================================
    void InitFXAA()
    {
        glGenTextures(1, &m_fxaaTex);
        glBindTexture(GL_TEXTURE_2D, m_fxaaTex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8,
            m_screenW, m_screenH, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

        glGenFramebuffers(1, &m_fxaaFBO);
        glBindFramebuffer(GL_FRAMEBUFFER, m_fxaaFBO);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
            GL_TEXTURE_2D, m_fxaaTex, 0);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
            Debug::Error("RenderSystem") << "FXAA FBO incomplete\n";

        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glBindTexture(GL_TEXTURE_2D, 0);
    }

    // =====================================================
    //  SSR shader
    //
    //  Algorithm:
    //    1. Reconstruct view-space position + normal from depth.
    //    2. Compute reflection vector.
    //    3. Ray-march in view space, projecting each step to
    //       get its UV and comparing to the depth buffer.
    //    4. On hit, binary-search refine the intersection.
    //    5. Sample the HDR colour buffer at the hit UV.
    //    6. Weight by metallic*(1-roughness) and fade at edges.
    //
    //  Output: RGBA16F where RGB = weighted reflection colour
    //          (added onto hdrColor in TonemapPass).
    // =====================================================
    void CompileSSRShader()
    {
        const char* quadVert = R"GLSL(
            #version 430 core
            layout(location = 0) in vec2 aPos;
            layout(location = 1) in vec2 aUV;
            out vec2 vUV;
            void main() { vUV = aUV; gl_Position = vec4(aPos, 0.0, 1.0); }
        )GLSL";

        const char* ssrFrag = R"GLSL(
            #version 430 core
            in  vec2 vUV;
            out vec4 FragColor;

            uniform sampler2D uDepthTex;    // GL_DEPTH_COMPONENT32F from HDR FBO
            uniform sampler2D uHDRColor;    // RGBA16F scene colour
            uniform mat4      uProjection;
            uniform mat4      uInvProj;
            uniform mat4      uView;
            uniform mat4      uInvView;
            uniform int       uMaxSteps;
            uniform float     uMaxDistance;
            uniform float     uStepSize;
            uniform int       uBinarySteps;
            uniform float     uRoughnessCutoff;
            uniform float     uFadeDistance;

            // Reconstruct view-space position from UV + raw depth [0,1]
            vec3 ViewPosFromDepth(vec2 uv, float depth)
            {
                vec4 ndc = vec4(uv * 2.0 - 1.0, depth * 2.0 - 1.0, 1.0);
                vec4 vp  = uInvProj * ndc;
                return vp.xyz / vp.w;
            }

            // Project a view-space position to screen UV
            vec2 ProjectToUV(vec3 vsPos)
            {
                vec4 clip = uProjection * vec4(vsPos, 1.0);
                clip.xyz /= clip.w;
                return clip.xy * 0.5 + 0.5;
            }

            void main()
            {
                float depth = texture(uDepthTex, vUV).r;

                // Skip sky
                if (depth >= 0.9999) { FragColor = vec4(0.0); return; }

                vec3 vsPos    = ViewPosFromDepth(vUV, depth);
                vec3 vsNormal = normalize(cross(dFdx(vsPos), dFdy(vsPos)));

                // For now metallic/roughness aren't passed per-pixel
                // (no G-buffer), so we use a fixed metallic weight of 1.0
                // and skip if the surface is clearly rough via a roughness
                // proxy (no separate MR texture available here).
                // When you wire up a G-buffer pass, replace these constants.
                float metallic  = 1.0;
                float roughness = 0.0;

                if (roughness > uRoughnessCutoff)
                { FragColor = vec4(0.0); return; }

                vec3 vsView = normalize(-vsPos); // toward camera in view space
                vec3 vsRefl = reflect(-vsView, vsNormal);

                // --- Ray march ---
                vec3  rayPos  = vsPos;
                vec3  rayStep = vsRefl * uStepSize;
                bool  hit     = false;
                vec2  hitUV   = vec2(0.0);

                for (int i = 0; i < uMaxSteps; ++i)
                {
                    rayPos += rayStep;

                    // Discard if ray escapes the view frustum depth range
                    if (-rayPos.z > uMaxDistance) break;

                    vec2  sampleUV = ProjectToUV(rayPos);
                    if (sampleUV.x < 0.0 || sampleUV.x > 1.0 ||
                        sampleUV.y < 0.0 || sampleUV.y > 1.0) break;

                    float sceneDepth = texture(uDepthTex, sampleUV).r;
                    vec3  scenePos   = ViewPosFromDepth(sampleUV, sceneDepth);

                    // Ray is behind geometry — we have a hit
                    if (rayPos.z < scenePos.z)
                    {
                        // Binary search to refine the intersection
                        vec3 lo = rayPos - rayStep;
                        vec3 hi = rayPos;
                        for (int b = 0; b < uBinarySteps; ++b)
                        {
                            vec3  mid    = (lo + hi) * 0.5;
                            vec2  midUV  = ProjectToUV(mid);
                            float midD   = texture(uDepthTex, midUV).r;
                            vec3  midPos = ViewPosFromDepth(midUV, midD);
                            if (mid.z < midPos.z) hi = mid;
                            else                  lo = mid;
                        }
                        hitUV = ProjectToUV((lo + hi) * 0.5);
                        hit   = true;
                        break;
                    }
                }

                if (!hit) { FragColor = vec4(0.0); return; }

                // --- Fade near screen edges ---
                vec2  edgeDist  = min(hitUV, 1.0 - hitUV);
                float edgeFade  = smoothstep(0.0, uFadeDistance, edgeDist.x)
                                * smoothstep(0.0, uFadeDistance, edgeDist.y);

                // --- Distance fade ---
                float distFade = 1.0 - clamp(length(rayPos - vsPos) / uMaxDistance,
                                             0.0, 1.0);

                float weight   = metallic * (1.0 - roughness) * edgeFade * distFade;
                vec3  reflCol  = texture(uHDRColor, hitUV).rgb;

                FragColor = vec4(reflCol * weight, weight);
            }
        )GLSL";

        m_ssrShader = LinkProgram({ CompileStage(GL_VERTEX_SHADER, quadVert),
                                    CompileStage(GL_FRAGMENT_SHADER, ssrFrag) });
    }

    // =====================================================
    //  FXAA shader  (Lottes / Timothy Lottes FXAA 3.11)
    //
    //  Operates on the gamma-corrected LDR buffer (m_fxaaTex).
    //  Converts each pixel to luminance, detects high-contrast
    //  edges, then blends along the edge direction by a
    //  sub-pixel offset to reduce stairstepping.
    //
    //  Inputs (uniforms):
    //    uLDRBuffer          — RGBA8 LDR texture
    //    uRcpFrame           — (1/w, 1/h) for texel-size offsets
    //    uEdgeThresholdMin   — minimum contrast to trigger AA
    //    uEdgeThreshold      — maximum contrast threshold
    //    uSubpixel           — sub-pixel smoothing strength
    // =====================================================
    void CompileFXAAShader()
    {
        const char* quadVert = R"GLSL(
            #version 430 core
            layout(location = 0) in vec2 aPos;
            layout(location = 1) in vec2 aUV;
            out vec2 vUV;
            void main() { vUV = aUV; gl_Position = vec4(aPos, 0.0, 1.0); }
        )GLSL";

        // Simplified FXAA 3.11 — single-pass, no pre-computed luma pass.
        // Luma is computed from the green channel (perceptual approximation).
        const char* fxaaFrag = R"GLSL(
            #version 430 core
            in  vec2 vUV;
            out vec4 FragColor;

            uniform sampler2D uLDRBuffer;
            uniform vec2      uRcpFrame;        // (1/screenW, 1/screenH)
            uniform float     uEdgeThresholdMin;
            uniform float     uEdgeThreshold;
            uniform float     uSubpixel;

            // BT.709 luma from RGB — using green-channel shortcut is fine
            // for edge detection; full coefficients give better accuracy.
            float Luma(vec3 rgb) { return dot(rgb, vec3(0.2126, 0.7152, 0.0722)); }

            void main()
            {
                // Sample the 3x3 neighbourhood for luma
                vec3 rgbM  = texture(uLDRBuffer, vUV).rgb;
                vec3 rgbN  = texture(uLDRBuffer, vUV + vec2( 0.0,  1.0) * uRcpFrame).rgb;
                vec3 rgbS  = texture(uLDRBuffer, vUV + vec2( 0.0, -1.0) * uRcpFrame).rgb;
                vec3 rgbE  = texture(uLDRBuffer, vUV + vec2( 1.0,  0.0) * uRcpFrame).rgb;
                vec3 rgbW  = texture(uLDRBuffer, vUV + vec2(-1.0,  0.0) * uRcpFrame).rgb;

                float lumaM = Luma(rgbM);
                float lumaN = Luma(rgbN);
                float lumaS = Luma(rgbS);
                float lumaE = Luma(rgbE);
                float lumaW = Luma(rgbW);

                float rangeMin = min(lumaM, min(min(lumaN, lumaS), min(lumaE, lumaW)));
                float rangeMax = max(lumaM, max(max(lumaN, lumaS), max(lumaE, lumaW)));
                float range    = rangeMax - rangeMin;

                // Skip pixels that don't meet the contrast threshold
                if (range < max(uEdgeThresholdMin, rangeMax * uEdgeThreshold))
                { FragColor = vec4(rgbM, 1.0); return; }

                // Diagonal corners
                vec3 rgbNW = texture(uLDRBuffer, vUV + vec2(-1.0,  1.0) * uRcpFrame).rgb;
                vec3 rgbNE = texture(uLDRBuffer, vUV + vec2( 1.0,  1.0) * uRcpFrame).rgb;
                vec3 rgbSW = texture(uLDRBuffer, vUV + vec2(-1.0, -1.0) * uRcpFrame).rgb;
                vec3 rgbSE = texture(uLDRBuffer, vUV + vec2( 1.0, -1.0) * uRcpFrame).rgb;

                float lumaNW = Luma(rgbNW);
                float lumaNE = Luma(rgbNE);
                float lumaSW = Luma(rgbSW);
                float lumaSE = Luma(rgbSE);

                // Sub-pixel blend factor
                float lumaAvg  = (lumaN + lumaS + lumaE + lumaW) * 2.0
                               + (lumaNW + lumaNE + lumaSW + lumaSE);
                lumaAvg /= 12.0;
                float subBlend = clamp(abs(lumaAvg - lumaM) / range, 0.0, 1.0);
                subBlend = subBlend * subBlend * uSubpixel;

                // Detect edge orientation
                float edgeH = abs(-2.0 * lumaW + lumaNW + lumaSW)
                            + abs(-2.0 * lumaM + lumaN  + lumaS ) * 2.0
                            + abs(-2.0 * lumaE + lumaNE + lumaSE);
                float edgeV = abs(-2.0 * lumaN + lumaNW + lumaNE)
                            + abs(-2.0 * lumaM + lumaW  + lumaE ) * 2.0
                            + abs(-2.0 * lumaS + lumaSW + lumaSE);
                bool isHoriz = edgeH >= edgeV;

                // Step one pixel perpendicular to the edge
                float stepLen  = isHoriz ? uRcpFrame.y : uRcpFrame.x;
                float luma1    = isHoriz ? lumaS : lumaW;
                float luma2    = isHoriz ? lumaN : lumaE;
                float grad1    = abs(luma1 - lumaM);
                float grad2    = abs(luma2 - lumaM);
                bool  side1    = grad1 >= grad2;
                float gradStep = max(grad1, grad2) * 0.25;
                float lumaEdge = side1 ? (luma1 + lumaM) * 0.5
                                       : (luma2 + lumaM) * 0.5;

                vec2  offStep = isHoriz ? vec2(uRcpFrame.x, 0.0)
                                        : vec2(0.0, uRcpFrame.y);
                vec2  offPerp = isHoriz ? vec2(0.0, stepLen * (side1 ? -1.0 : 1.0))
                                        : vec2(stepLen * (side1 ? -1.0 : 1.0), 0.0);

                vec2 uv1 = vUV + offPerp - offStep;
                vec2 uv2 = vUV + offPerp + offStep;

                // Walk along the edge in both directions to find its ends
                // (simplified: fixed 8-step search)
                float done1 = 0.0, done2 = 0.0;
                float end1 = 0.0, end2 = 0.0;
                for (int i = 0; i < 8; ++i)
                {
                    if (done1 < 0.5) { end1 = Luma(texture(uLDRBuffer, uv1).rgb) - lumaEdge; uv1 -= offStep; }
                    if (done2 < 0.5) { end2 = Luma(texture(uLDRBuffer, uv2).rgb) - lumaEdge; uv2 += offStep; }
                    done1 = step(gradStep, abs(end1));
                    done2 = step(gradStep, abs(end2));
                    if (done1 > 0.5 && done2 > 0.5) break;
                }

                float dist1 = isHoriz ? (vUV.x - (uv1.x + offStep.x)) : (vUV.y - (uv1.y + offStep.y));
                float dist2 = isHoriz ? ((uv2.x - offStep.x) - vUV.x)  : ((uv2.y - offStep.y) - vUV.y);
                bool  side  = dist1 < dist2;
                float edgeBlend = 0.5 - min(dist1, dist2) / (dist1 + dist2);

                bool correctDir = (side ? end1 : end2) < 0.0
                               != (lumaM < lumaEdge);
                float pixBlend = correctDir ? edgeBlend : 0.0;

                float finalBlend = max(subBlend, pixBlend);
                vec2  blendUV   = vUV + offPerp * finalBlend;

                FragColor = vec4(texture(uLDRBuffer, blendUV).rgb, 1.0);
            }
        )GLSL";

        m_fxaaShader = LinkProgram({ CompileStage(GL_VERTEX_SHADER, quadVert),
                                     CompileStage(GL_FRAGMENT_SHADER, fxaaFrag) });
    }

    // =====================================================
    //  SSRPass
    //  Renders screen-space reflections into m_ssrTex.
    //  TonemapPass additively composites it onto the HDR
    //  buffer before tonemapping.
    // =====================================================
    void SSRPass(const glm::mat4& view, const glm::mat4& projection)
    {
        const auto& rs = RenderSettings::instance();

        glBindFramebuffer(GL_FRAMEBUFFER, m_ssrFBO);
        glViewport(0, 0, m_screenW, m_screenH);
        glDisable(GL_DEPTH_TEST);
        glClear(GL_COLOR_BUFFER_BIT);

        glUseProgram(m_ssrShader);

        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, m_hdrDepthTex);
        glUniform1i(glGetUniformLocation(m_ssrShader, "uDepthTex"), 0);

        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, m_hdrColorTex);
        glUniform1i(glGetUniformLocation(m_ssrShader, "uHDRColor"), 1);

        glm::mat4 invProj = glm::inverse(projection);
        glm::mat4 invView = glm::inverse(view);

        glUniformMatrix4fv(glGetUniformLocation(m_ssrShader, "uProjection"),
            1, GL_FALSE, glm::value_ptr(projection));
        glUniformMatrix4fv(glGetUniformLocation(m_ssrShader, "uInvProj"),
            1, GL_FALSE, glm::value_ptr(invProj));
        glUniformMatrix4fv(glGetUniformLocation(m_ssrShader, "uView"),
            1, GL_FALSE, glm::value_ptr(view));
        glUniformMatrix4fv(glGetUniformLocation(m_ssrShader, "uInvView"),
            1, GL_FALSE, glm::value_ptr(invView));

        glUniform1i(glGetUniformLocation(m_ssrShader, "uMaxSteps"),
            rs.getSSRMaxSteps());
        glUniform1f(glGetUniformLocation(m_ssrShader, "uMaxDistance"),
            rs.getSSRMaxDistance());
        glUniform1f(glGetUniformLocation(m_ssrShader, "uStepSize"),
            rs.getSSRStepSize());
        glUniform1i(glGetUniformLocation(m_ssrShader, "uBinarySteps"),
            rs.getSSRBinarySteps());
        glUniform1f(glGetUniformLocation(m_ssrShader, "uRoughnessCutoff"),
            rs.getSSRRoughnessCutoff());
        glUniform1f(glGetUniformLocation(m_ssrShader, "uFadeDistance"),
            rs.getSSRFadeDistance());

        glBindVertexArray(m_quadVAO);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        glBindVertexArray(0);

        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glEnable(GL_DEPTH_TEST);
    }

    // =====================================================
    //  FXAAPass
    //  Reads the LDR-tonemapped m_fxaaTex, applies FXAA,
    //  and writes to the default framebuffer (FB 0).
    // =====================================================
    void FXAAPass()
    {
        const auto& rs = RenderSettings::instance();

        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glViewport(0, 0, m_screenW, m_screenH);
        glDisable(GL_DEPTH_TEST);
        glClear(GL_COLOR_BUFFER_BIT);

        glUseProgram(m_fxaaShader);

        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, m_fxaaTex);
        glUniform1i(glGetUniformLocation(m_fxaaShader, "uLDRBuffer"), 0);

        glUniform2f(glGetUniformLocation(m_fxaaShader, "uRcpFrame"),
            1.0f / (float)m_screenW, 1.0f / (float)m_screenH);
        glUniform1f(glGetUniformLocation(m_fxaaShader, "uEdgeThresholdMin"),
            rs.getFXAAEdgeThresholdMin());
        glUniform1f(glGetUniformLocation(m_fxaaShader, "uEdgeThreshold"),
            rs.getFXAAEdgeThreshold());
        glUniform1f(glGetUniformLocation(m_fxaaShader, "uSubpixel"),
            rs.getFXAASubpixel());

        glBindVertexArray(m_quadVAO);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        glBindVertexArray(0);

        glEnable(GL_DEPTH_TEST);
    }

};

#endif // RENDER_SYSTEM_HPP