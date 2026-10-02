#ifndef RENDER_SYSTEM_HPP
#define RENDER_SYSTEM_HPP

#include "ecs/ecs.hpp"
#include "ecs/ecs_common.hpp"
#include "Utils/Debug/Debug.hpp"
#include "OpenGL/Mesh.hpp"
#include "OpenGL/Material.hpp"
#include "OpenGL/OpenGLIncludes.hpp"
#include "RenderSettings.hpp"
#include <glm/gtc/type_ptr.hpp>
#include <vector>
#include <string>
#include <iostream>
#include <cstdlib>
#include <cmath>
#include <algorithm>
#include <filesystem>
#include <atomic>
#include <unordered_map>

#include "OpenGL/Particles/ParticleSystem.hpp"


struct GPUPointLight {
    glm::vec4 posRadius;      // xyz = world pos,  w = radius
    glm::vec4 colorIntensity; // rgb = color,       a = intensity
};

// GPU layout for the directional light uniform block, sent via a UBO (binding 2), padded to std140 rules.
struct GPUDirLight {
    glm::vec4 directionIntensity; // xyz = world-space direction (normalised, toward scene), w = intensity
    glm::vec4 colorEnabled;       // rgb = color, a = 1.0 if enabled else 0.0
    glm::mat4 lightSpaceMatrix;   // ortho VP for shadow map (identity when shadows disabled)
};

struct GPUShadowData {
    glm::mat4 lightSpaceMatrices[6]; // one per cube face
    int       lightIndex;
    float     farPlane;
    int       pad[2];
};

// Concrete type for the mesh query used across passes.
using MeshQuery = decltype(
    std::declval<EntityManager>().CreateQuery<MeshComponent, Transform>());

// Per-frame pipeline: [GBufferPass → LinearDepthPass, only if SSAO or SSR is on] → SSAOPass → CollectLightsPass → ShadowPass → DirShadowPass → ShadingPass → AdditivePass → SSRPass → particles → MotionBlurPass → BloomPass → TonemapPass → FXAAPass [→ CopyPass (nearest), FSRPass or NISPass, per the upscale mode].
class RenderSystem : public ISystem {
public:

    RenderSystem() { drawsFrame = true; }

	bool needsReinit = false;  // Set true to reinitialise all GPU resources next frame

    // screenW/H: internal render resolution (every screen-sized target). outputW/H: the window's framebuffer, which
    // only FXAAPass draws to, scaling the render resolution image up or down to it.
    void Init(int screenW, int screenH, int outputW, int outputH);
    // Only recreates the targets when the render resolution changed; a new output size alone is free.
    void Resize(int screenW, int screenH, int outputW, int outputH);
    void ReInitShadows();

    void Update(EntityManager& entityManager,
        std::vector<EventEntry>& events,
        bool isServer,
        float deltaTime) override;

    ~RenderSystem();

    // Every render target into Render/<timestamp>/ (or dumpDir if given). Must run after a frame, before the swap.
    void DumpBuffers(std::string dumpDir = {}) const;
    // Next Update() saves the frame after each pass (stage_NN_*.png) and then DumpBuffers(), all in one folder.
    void RequestDebugDump() { m_debugDumpRequested = true; }
    // Next Update() saves only the finished frame (no UI, which is drawn after) as Render/<timestamp>.png.
    void RequestFinalFrameDump() { m_finalDumpRequested = true; }

    void SetParticleSystem(ParticleSystem* ps) { m_particleSystem = ps; }

    int GetScreenWidth() const { return m_screenW; }
    int GetScreenHeight() const { return m_screenH; }
    int GetOutputWidth() const { return m_outputW; }
    int GetOutputHeight() const { return m_outputH; }

private:
    int MAX_LIGHTS = 512;
    int MAX_SHADOW_LIGHTS = 8;

    int m_screenW = 0, m_screenH = 0;   // render resolution
    int m_outputW = 0, m_outputH = 0;   // window framebuffer
    int m_lightCount = 0;

    ParticleSystem* m_particleSystem = nullptr;

    // Point light SSBO (binding 0)
    GLuint m_lightSSBO = 0;

    // Point light shadow resources
    GLuint m_shadowCubeArray = 0;
    GLuint m_shadowFBO = 0;
    GLuint m_shadowShader = 0;
    GLuint m_shadowDataSSBO = 0;
    int    m_shadowRes = 512;
    int    m_shadowCount = 0;

    // Directional light UBO (binding 2), one GPUDirLight struct; colorEnabled.a == 0 means the shader skips the term.
    GLuint m_dirLightUBO = 0;

    // Directional light shadow map: a single DEPTH32F texture + FBO, resolution shared with point lights via getShadowResolution().
    GLuint m_dirShadowTex = 0;   // sampler2D, DEPTH32F
    GLuint m_dirShadowFBO = 0;
    GLuint m_dirShadowShader = 0;

    GPUDirLight m_cpuDirLight{};  // cached copy of the directional light for CPU-side use (e.g. particles)

    // GBuffer framebuffer (layout shared with GBufferVariant.hpp). Only filled when SSAO or SSR needs it. Depth is a
    // sampleable texture (not an RBO) because SSAO/SSR reconstruct view positions from it.
    GLuint m_gbufferFBO = 0;
    GLuint m_gbufferNormalTex = 0;     // RGBA16F: xyz = view-space normal, w = perceptual roughness
    GLuint m_gbufferMaterialTex = 0;   // RGBA8:   r = metalness, g = 1 if the material has its own env reflection
    GLuint m_gbufferAlbedoTex = 0;     // RGBA8:   rgb = base colour (SSR uses it as the metals' F0)
    GLuint m_gbufferVelocityTex = 0;   // RG16F:   object-only screen motion this frame (UV units), camera motion excluded
    GLuint m_gbufferDepthTex = 0;

    // SSAO and SSR each run at the render resolution divided by their own setting (1, 2 or 4; see RenderSettings'
    // ResolutionScale). Sizes are rounded up, so reduced pixel p covers full pixels s*p .. s*p+s-1 and reads pixel s*p.
    // The scales the targets were built with; Update() rebuilds them when the settings change.
    int m_ssaoScale = 2, m_ssaoW = 1, m_ssaoH = 1;
    int m_ssrScale = 2,  m_ssrW = 1,  m_ssrH = 1;

    // View-space Z (R32F) of full pixel s*p at the SSAO scale, background = -1e6. Built once per frame from the
    // GBuffer depth so SSAO/SSR samples cost one fetch instead of a fetch plus an inverse-projection matrix multiply.
    // Motion blur reads it too. SSR gets its own copy at its scale only when the two scales differ (see SSRLinearDepth).
    GLuint m_linearDepthFBO = 0;
    GLuint m_linearDepthTex = 0;
    GLuint m_ssrLinearDepthFBO = 0;
    GLuint m_ssrLinearDepthTex = 0;
    GLuint SSRLinearDepthTex() const { return m_ssrLinearDepthTex ? m_ssrLinearDepthTex : m_linearDepthTex; }

    // SSAO: raw AO (noisy, 4x4 rotation pattern) then a depth-aware 4x4 blur that the shading pass samples (bilinear
    // upscale) on unit 7. When SSAO is off the blurred target is cleared to 1.0, so materials never branch on it.
    GLuint m_ssaoFBO = 0;
    GLuint m_ssaoTex = 0;
    GLuint m_ssaoBlurFBO = 0;
    GLuint m_ssaoBlurTex = 0;
    std::vector<glm::vec3> m_ssaoKernel;

    // SSR: reduced-res mipmapped copy of the HDR scene (rougher surfaces read blurrier mips), reduced-res trace result
    // (premultiplied colour + replace alpha), then a depth-aware upscale composite into m_hdrFBO.
    GLuint m_ssrSceneFBO = 0;
    GLuint m_ssrSceneTex = 0;
    int    m_ssrSceneMips = 1;
    GLuint m_ssrTraceFBO = 0;
    GLuint m_ssrTraceTex = 0;

    // Motion blur (McGuire-style reconstruction): full-res velocity (camera + object, as a blur radius in pixels,
    // clamped to one tile), per-tile max, 3x3 neighbour max of that, then a gather pass reading a copy of the HDR scene.
    int    m_mbTile = 16;              // tile size in pixels = max blur radius
    int    m_mbTilesW = 1, m_mbTilesH = 1;
    GLuint m_mbVelocityFBO = 0;
    GLuint m_mbVelocityTex = 0;
    GLuint m_mbTileMaxFBO = 0;
    GLuint m_mbTileMaxTex = 0;
    GLuint m_mbNeighborMaxFBO = 0;
    GLuint m_mbNeighborMaxTex = 0;
    GLuint m_mbSourceFBO = 0;
    GLuint m_mbSourceTex = 0;

    // Previous frame's transforms for velocities: camera view-projection, and each mesh entity's model matrix
    // (rebuilt every GBufferPass, so destroyed entities drop out).
    glm::mat4 m_prevViewProjection{ 1.0f };
    bool      m_hasPrevViewProjection = false;
    std::unordered_map<Entity, glm::mat4> m_prevModels;
    std::unordered_map<Entity, glm::mat4> m_currModels;

    // MSAA framebuffer
    GLuint m_msaaFBO = 0;
    GLuint m_msaaColorTex = 0;
    GLuint m_msaaDepthTex = 0;
    int    m_msaaSamples = 1;

    // HDR framebuffer + screen-quad
    GLuint m_hdrFBO = 0;
    GLuint m_hdrColorTex = 0;
    GLuint m_hdrDepthTex = 0;
    GLuint m_quadVAO = 0;
    GLuint m_quadVBO = 0;

    mutable std::atomic<bool> m_debugDumpRequested{ false };
    std::atomic<bool>         m_finalDumpRequested{ false };
    void DumpFinalFrame() const;

    // Per-stage dump of the frame being rendered: folder for this frame (empty = not dumping) and next stage number.
    // HDRBloom: tex (HDR scene) + m_bloomPingTex * bloom strength, the sum the tonemap shader makes and never stores.
    enum class StageImage { HDR, HDRBloom, LDR, Window, Grey, Normals, Depth };
    std::string m_stageDumpDir;
    int         m_stageDumpIndex = 0;
    static std::string MakeDumpDir();
    static std::string DumpTimestamp();
    // Saves tex (w x h, read as `kind`) as stage_NN_<name>.png when a stage dump is active. Restores the GL bindings it
    // touches, so it can run between passes. Window reads the default framebuffer's back buffer at the output size.
    void DumpStage(const char* name, StageImage kind, GLuint tex = 0, int w = 0, int h = 0);
    // Bloom resources
    GLuint m_bloomThreshFBO = 0;
    GLuint m_bloomThreshTex = 0;
    GLuint m_bloomPingFBO = 0;
    GLuint m_bloomPingTex = 0;
    GLuint m_bloomPongFBO = 0;
    GLuint m_bloomPongTex = 0;

    // LDR framebuffer
    GLuint m_ldrFBO = 0;
    GLuint m_ldrTex = 0;

    // Scaling targets for the Nearest, FSR 1 and NIS modes (Bilinear needs none). Created on first use and recreated
    // whenever the render or output size changes, so they cost nothing in Bilinear. Pre-scale: FXAA's output at render
    // resolution (anti-aliasing must run before the scale); upscale output (FSR/NIS only): the upscaled image at output
    // resolution, which RCAS sharpens into the window (FSR) or which is copied to it (NIS writes it as an image, and
    // the window can't be one).
    GLuint m_preScaleFBO = 0;
    GLuint m_preScaleTex = 0;
    int    m_preScaleW = 0, m_preScaleH = 0;
    GLuint m_upscaleOutFBO = 0;
    GLuint m_upscaleOutTex = 0;
    int    m_upscaleOutW = 0, m_upscaleOutH = 0;

    // NVIDIA Image Scaling: coefficient tables (RGBA32F, 2 x 64: 8 taps per phase), constants UBO (NISConfig) and a
    // linear/clamp sampler object for the input, whatever filter its texture has. Block size follows the GPU vendor
    // (NISOptimizer); it is baked into the shader, so it's picked in CompileNISShader.
    GLuint m_nisCoefScaleTex = 0;
    GLuint m_nisCoefUsmTex = 0;
    GLuint m_nisConfigUBO = 0;
    GLuint m_nisSampler = 0;
    int    m_nisBlockW = 32, m_nisBlockH = 24;

    // Shader programs
    GLuint m_gbufferShader = 0;
    GLuint m_tonemapShader = 0;
    GLuint m_bloomThreshShader = 0;
    GLuint m_bloomKawaseShader = 0;
    GLuint m_fxaaShader = 0;
    GLuint m_ssaoShader = 0;
    GLuint m_ssaoBlurShader = 0;
    GLuint m_ssrShader = 0;
    GLuint m_ssrCopyShader = 0;
    GLuint m_ssrCompositeShader = 0;
    GLuint m_linearDepthShader = 0;
    GLuint m_mbVelocityShader = 0;
    GLuint m_mbTileMaxShader = 0;
    GLuint m_mbNeighborMaxShader = 0;
    GLuint m_mbGatherShader = 0;
    GLuint m_fsrEasuShader = 0;
    GLuint m_fsrRcasShader = 0;
    GLuint m_nisShader = 0;    // compute
    GLuint m_copyShader = 0;   // final copy to the window without FXAA / Nearest scaling (see CompileCopyShader)

    // Initialisation helpers
    void InitLightSSBO();
    void InitShadowCubeArray();
    void InitDirLightUBO();
    void InitDirShadowMap();
    void InitGBufferFBO();
    void InitMSAAFBO();
    void InitHDRFBO();
    void InitBloom();
    void InitLDRFBO();
    void InitScreenSpace(); // reduced-res linear depth + SSAO + SSR targets (at the settings' scales), motion blur
    void DeleteScreenSpace();
    void InitScreenQuad();
    // (Re)creates the pre-scale target, and the upscale output one if withOutput, when missing or the render/output
    // size changed.
    void EnsureScaleTargets(bool withOutput);
    void DeleteScaleTargets();

    void ResolveMSAA();

    // Shader compilation helpers
    static GLuint CompileStage(GLenum type, const char* src);
    static GLuint LinkProgram(std::initializer_list<GLuint> stages);

    void CompileGBufferShader();
    void CompileShadowShader();
    void CompileDirShadowShader();
    void CompileTonemapShader();
    void CompileBloomShaders();
    void CompileFXAAShader();
    void CompileSSAOShaders(); // also the linear depth shader both effects share
    void CompileSSRShader();
    void CompileMotionBlurShaders();
    void CompileFSRShaders();
    void CompileNISShader();   // also the coefficient textures, UBO and sampler it uses
    void DeleteNIS();
    void CompileCopyShader();

    // Per-frame passes
    // Clears targetFBO and draws sourceTex (render resolution) over all of it with the copy shader.
    void CopyPass(GLuint sourceTex, GLuint targetFBO, int targetW, int targetH, bool nearest);
    void GBufferPass(EntityManager::Query<MeshComponent, Transform>& meshQuery,
        const glm::mat4& view, const glm::mat4& projection);
    // View-space Z of every scale-th pixel into fbo (w x h).
    void LinearDepthPass(const glm::mat4& projection, GLuint fbo, int w, int h, int scale);
    void SSAOPass(const glm::mat4& projection);
    // Fills the blurred AO target with 1.0 (no occlusion), what the shading pass reads when SSAO is off.
    void ClearSSAO();
    void SSRPass(const glm::mat4& projection);
    void MotionBlurPass(const glm::mat4& viewProjection, float deltaTime);
    void CollectLightsPass(EntityManager& em);  // also handles directional light
    void ShadowPass(EntityManager& em, EntityManager::Query<MeshComponent, Transform>& meshQuery);
    void DirShadowPass(EntityManager::Query<MeshComponent, Transform>& meshQuery,
        const glm::vec3& cameraPos);
    void ShadingPass(EntityManager::Query<MeshComponent, Transform>& meshQuery,
        const glm::mat4& view, const glm::mat4& projection,
        const glm::vec3& cameraPos);
    void AdditivePass(EntityManager::Query<MeshComponent, Transform>& meshQuery,
        const glm::mat4& view, const glm::mat4& projection,
        const glm::vec3& cameraPos);
    void BloomPass();
    void TonemapPass();
    // Final pass to the window in Bilinear mode. Draws to targetFBO at its size (render resolution for the Nearest/FSR
    // pre-scale target, output otherwise).
    void FXAAPass(GLuint targetFBO, int targetW, int targetH);
    // EASU (render -> output resolution) then RCAS into the window. Replaces the bilinear scale in FXAAPass.
    void FSRPass(GLuint inputTex);
    // NVScaler compute pass (render -> output resolution) into the upscale output target, then a copy into the window.
    void NISPass(GLuint inputTex);
    // Upscale mode actually used this frame: the setting, except FSR1/NIS fall back to Bilinear when they can't apply
    // (native or higher render resolution, beyond 2x for NIS, shaders failed) and nothing but Bilinear is needed at
    // native resolution.
    UpscaleMode ActiveUpscaleMode() const;
};

#endif // RENDER_SYSTEM_HPP