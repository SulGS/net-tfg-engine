#include "RenderSystem.hpp"

void RenderSystem::Init(int screenW, int screenH, int outputW, int outputH)
{
    const auto& rs = RenderSettings::instance();
    MAX_LIGHTS = rs.getMaxLights();
    MAX_SHADOW_LIGHTS = rs.getMaxShadowLights();
    m_msaaSamples = rs.getMsaaSamples();

    GLint maxSamples = 1;
    glGetIntegerv(GL_MAX_SAMPLES, &maxSamples);
    if (m_msaaSamples > maxSamples) {
        Debug::Warning("RenderSystem") << "MSAA x" << m_msaaSamples
            << " not supported; clamping to x" << maxSamples << "\n";
        m_msaaSamples = maxSamples;
    }

    m_screenW = screenW;
    m_screenH = screenH;
    m_outputW = outputW;
    m_outputH = outputH;

    glEnable(GL_TEXTURE_CUBE_MAP_SEAMLESS);

    InitMSAAFBO();
    InitHDRFBO();
    InitGBufferFBO();
    InitLDRFBO();

    {
        const uint8_t white = 255;
        glGenTextures(1, &m_whiteTex);
        glBindTexture(GL_TEXTURE_2D, m_whiteTex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, 1, 1, 0, GL_RED, GL_UNSIGNED_BYTE, &white);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glBindTexture(GL_TEXTURE_2D, 0);
    }
    EnsureEffectTargets();

    InitLightSSBO();
    InitShadowCubeArray();
    InitDirLightUBO();
    InitDirShadowMap();
    CompileGBufferShader();
    CompileShadowShader();
    CompileDirShadowShader();
    CompileTonemapShader();
    CompileBloomShaders();
    CompileFXAAShader();
    CompileSSAOShaders();
    CompileSSRShader();
    CompileMotionBlurShaders();
    CompileFSRShaders();
    CompileNISShader();
    CompileCopyShader();
    InitScreenQuad();
}

void RenderSystem::Resize(int screenW, int screenH, int outputW, int outputH)
{
    // A 0-sized target (minimized window) makes every FBO incomplete.
    if (screenW <= 0 || screenH <= 0 || outputW <= 0 || outputH <= 0) return;

    m_outputW = outputW;
    m_outputH = outputH;

    // Same render resolution (e.g. only the window changed with a fixed render height... or nothing did):
    // the targets are still valid, FXAAPass just scales to the new output (FSRPass recreates its own output target).
    if (screenW == m_screenW && screenH == m_screenH) return;

    m_screenW = screenW;
    m_screenH = screenH;

    glDeleteFramebuffers(1, &m_msaaFBO);   m_msaaFBO = 0;
    glDeleteTextures(1, &m_msaaColorTex);  m_msaaColorTex = 0;
    glDeleteTextures(1, &m_msaaDepthTex);  m_msaaDepthTex = 0;
    InitMSAAFBO();

    glDeleteFramebuffers(1, &m_hdrFBO);   m_hdrFBO = 0;
    glDeleteTextures(1, &m_hdrColorTex);  m_hdrColorTex = 0;
    glDeleteTextures(1, &m_hdrDepthTex);  m_hdrDepthTex = 0;
    InitHDRFBO();

    glDeleteFramebuffers(1, &m_gbufferFBO);      m_gbufferFBO = 0;
    glDeleteTextures(1, &m_gbufferNormalTex);    m_gbufferNormalTex = 0;
    glDeleteTextures(1, &m_gbufferMaterialTex);  m_gbufferMaterialTex = 0;
    glDeleteTextures(1, &m_gbufferAlbedoTex);    m_gbufferAlbedoTex = 0;
    glDeleteTextures(1, &m_gbufferVelocityTex);  m_gbufferVelocityTex = 0;
    glDeleteTextures(1, &m_gbufferDepthTex);     m_gbufferDepthTex = 0;
    InitGBufferFBO();

    // Recreated at the new size, if their effects are on.
    DeleteScreenSpace();
    DeleteBloom();
    EnsureEffectTargets();

    glDeleteFramebuffers(1, &m_ldrFBO); m_ldrFBO = 0;
    glDeleteTextures(1, &m_ldrTex);     m_ldrTex = 0;
    InitLDRFBO();

    // Dir shadow map resolution does not depend on screen size � no resize needed.
}

void RenderSystem::ReInitShadows()
{
    // Point light shadows
    glDeleteFramebuffers(1, &m_shadowFBO);   m_shadowFBO = 0;
    glDeleteTextures(1, &m_shadowCubeArray); m_shadowCubeArray = 0;
    glDeleteSamplers(1, &m_shadowCmpSampler); m_shadowCmpSampler = 0;
    glDeleteBuffers(1, &m_shadowDataSSBO);   m_shadowDataSSBO = 0;
    InitShadowCubeArray();

    // Directional light shadow (same resolution setting)
    glDeleteFramebuffers(1, &m_dirShadowFBO); m_dirShadowFBO = 0;
    glDeleteTextures(1, &m_dirShadowTex);     m_dirShadowTex = 0;
    glDeleteSamplers(1, &m_dirShadowCmpSampler); m_dirShadowCmpSampler = 0;
    InitDirShadowMap();
}

void RenderSystem::Update(EntityManager& entityManager,
    std::vector<EventEntry>& events,
    bool /*isServer*/,
    float deltaTime)
{
	if (needsReinit) {
        // Free the current set first: Init() generates every object anew (shadow maps included, at the new settings).
        ReleaseGPUResources();
        Init(m_screenW, m_screenH, m_outputW, m_outputH);
        needsReinit = false;
	}
    EnsureEffectTargets();



    if (m_hdrFBO == 0) {
        Debug::Error("RenderSystem") << "Call Init() before first Update()\n";
        return;
    }

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
        // Nothing else writes the window this frame: clear it so the UI isn't drawn over the previous one.
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glViewport(0, 0, m_outputW, m_outputH);
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        entityManager.releaseMutex();
        return;
    }

    glm::mat4 view = activeCamera->getViewMatrix();
    glm::mat4 projection = activeCamera->getProjectionMatrix();
    glm::vec3 cameraPos = cameraTransform->getPosition();

    auto meshQuery = entityManager.CreateQuery<MeshComponent, Transform>();

    const auto& rs = RenderSettings::instance();

    // Debug dump (F12): the frame after every pass below as stage_NN_*.png, then every target (DumpBuffers) at the end.
    m_stageDumpDir.clear();
    m_stageDumpIndex = 0;
    if (m_debugDumpRequested.exchange(false))
        m_stageDumpDir = MakeDumpDir();

    // SSAO/SSR resolution changed in the settings: rebuild their targets (cheap, only screen-space ones).
    // Only when they exist (EnsureEffectTargets builds them at the current scales when an effect is switched on).
    if (m_linearDepthTex && (rs.getSSAOResolutionScale() != m_ssaoScale || rs.getSSRResolutionScale() != m_ssrScale)) {
        DeleteScreenSpace();
        InitScreenSpace();
    }

    // The GBuffer only feeds SSAO/SSR/motion blur: with all of them off, skip the whole extra geometry pass.
    const bool screenSpace = rs.getSSAOEnabled() || rs.getSSREnabled() || rs.getMotionBlurEnabled();
    if (screenSpace) {
        GBufferPass(meshQuery, view, projection);
        // At the SSAO scale: SSAO and motion blur read it, and SSR too when its scale is the same.
        LinearDepthPass(projection, m_linearDepthFBO, m_ssaoW, m_ssaoH, m_ssaoScale);
        if (m_ssrLinearDepthFBO && rs.getSSREnabled())
            LinearDepthPass(projection, m_ssrLinearDepthFBO, m_ssrW, m_ssrH, m_ssrScale);
        DumpStage("gbuffer_normal", StageImage::Normals, m_gbufferNormalTex, m_screenW, m_screenH);
        DumpStage("gbuffer_depth", StageImage::Depth, m_gbufferDepthTex, m_screenW, m_screenH);
    }

    // Needs only the GBuffer; ShadingPass samples its result (all 1.0 when SSAO is off).
    SSAOPass(projection);
    if (rs.getSSAOEnabled())
        DumpStage("ssao_buffer", StageImage::Grey, m_ssaoBlurTex, m_ssaoW, m_ssaoH);

    // CollectLightsPass now handles both point lights and the directional light.
    CollectLightsPass(entityManager);

    if (rs.getPointShadowsEnabled())
        ShadowPass(entityManager, meshQuery);   // point light cubemap shadows
    else
        m_shadowCount = 0;

    if (rs.getDirShadowsEnabled())
        DirShadowPass(meshQuery, view, projection,          // directional light cascaded shadows
            activeCamera->getNearPlane(), activeCamera->getFarPlane());

    // SSAO has no pass of its own on the scene: the materials multiply their ambient by it while shading. To see what it
    // adds, the dump frame is shaded once more first with the AO at 1.0 (as if it were off), then SSAO is recomputed.
    if (!m_stageDumpDir.empty() && rs.getSSAOEnabled()) {
        ClearSSAO();
        ShadingPass(meshQuery, view, projection, cameraPos);
        DumpStage("shading_no_ssao", StageImage::HDR, m_hdrColorTex, m_screenW, m_screenH);
        SSAOPass(projection);
    }

    ShadingPass(meshQuery, view, projection, cameraPos);
    DumpStage(rs.getSSAOEnabled() ? "shading_ssao" : "shading", StageImage::HDR, m_hdrColorTex, m_screenW, m_screenH);

    // Before the particles so their distortion pass (which copies the scene) sees the beams too.
    AdditivePass(meshQuery, view, projection, cameraPos);
    DumpStage("additive", StageImage::HDR, m_hdrColorTex, m_screenW, m_screenH);

    // After the beams so they show up in reflections; before the particles, which aren't in the GBuffer and would
    // otherwise be reflected as if they were the opaque surface behind them.
    if (rs.getSSREnabled()) {
        SSRPass(projection);
        DumpStage("ssr", StageImage::HDR, m_hdrColorTex, m_screenW, m_screenH);
    }

    if (m_particleSystem) {
        m_particleSystem->Draw(view, projection);
        DumpStage("particles", StageImage::HDR, m_hdrColorTex, m_screenW, m_screenH);
    }

    // On the finished HDR scene (particles and beams included) and before bloom, so bright streaks bloom too.
    const glm::mat4 viewProjection = projection * view;
    if (rs.getMotionBlurEnabled()) {
        MotionBlurPass(viewProjection, deltaTime);
        DumpStage("motion_blur", StageImage::HDR, m_hdrColorTex, m_screenW, m_screenH);
    }
    m_prevViewProjection = viewProjection;
    m_hasPrevViewProjection = true;

    // Bloom doesn't touch the scene (the tonemap shader adds it): dumped as the blurred bloom alone (half res), then
    // the scene with it added the way the tonemap does.
    if (rs.getBloomEnabled()) {
        BloomPass();
        DumpStage("bloom_blur", StageImage::HDR, m_bloomPingTex, std::max(1, m_screenW / 2), std::max(1, m_screenH / 2));
        DumpStage("bloom", StageImage::HDRBloom, m_hdrColorTex, m_screenW, m_screenH);
    }

    TonemapPass();
    DumpStage("tonemap", StageImage::LDR, m_ldrTex, m_screenW, m_screenH);

    // Bilinear: FXAA straight to the window, scaling as it samples. Nearest/FSR/NIS: FXAA (if on) at render resolution
    // into the pre-scale target first, so the scaler works on the already anti-aliased image.
    const UpscaleMode upscale = ActiveUpscaleMode();
    if (upscale == UpscaleMode::Bilinear) {
        if (m_preScaleTex || m_upscaleOutTex) DeleteScaleTargets();   // just switched to Bilinear or native: free them
        FXAAPass(0, m_outputW, m_outputH);
        DumpStage(rs.getFXAAEnabled() ? "fxaa_scale_window" : "scale_window", StageImage::Window);
    } else {
        EnsureScaleTargets(upscale != UpscaleMode::Nearest);

        GLuint scaleTex = m_ldrTex;
        if (rs.getFXAAEnabled()) {
            FXAAPass(m_preScaleFBO, m_screenW, m_screenH);
            scaleTex = m_preScaleTex;
            DumpStage("fxaa", StageImage::LDR, m_preScaleTex, m_screenW, m_screenH);
        }

        switch (upscale) {
        case UpscaleMode::FSR1: FSRPass(scaleTex); break;
        case UpscaleMode::NIS:  NISPass(scaleTex); break;
        default:                CopyPass(scaleTex, 0, m_outputW, m_outputH, true); break;
        }
        DumpStage(upscale == UpscaleMode::FSR1 ? "fsr_window" : upscale == UpscaleMode::NIS ? "nis_window" : "nearest_window",
            StageImage::Window);
    }

    if (!m_stageDumpDir.empty()) {
        DumpBuffers(m_stageDumpDir);
        m_stageDumpDir.clear();
    }

    // F11: the finished frame alone, before the UI is drawn over it.
    if (m_finalDumpRequested.exchange(false))
        DumpFinalFrame();

    entityManager.releaseMutex();
}

RenderSystem::~RenderSystem()
{
    ReleaseGPUResources();
}

void RenderSystem::ReleaseGPUResources()
{
    // Every name goes back to 0: Init() regenerates them, and a stale non-zero name deleted again later would destroy
    // whatever object the driver has handed that name to since (another world's, typically).
    auto tex  = [](GLuint& n) { if (n) glDeleteTextures(1, &n);      n = 0; };
    auto fbo  = [](GLuint& n) { if (n) glDeleteFramebuffers(1, &n);  n = 0; };
    auto buf  = [](GLuint& n) { if (n) glDeleteBuffers(1, &n);       n = 0; };
    auto smp  = [](GLuint& n) { if (n) glDeleteSamplers(1, &n);      n = 0; };
    auto prog = [](GLuint& n) { if (n) glDeleteProgram(n);           n = 0; };

    // Point / directional light shadows
    buf(m_lightSSBO);
    fbo(m_shadowFBO);
    tex(m_shadowCubeArray);
    smp(m_shadowCmpSampler);
    buf(m_shadowDataSSBO);
    prog(m_shadowShader);
    buf(m_dirLightUBO);
    fbo(m_dirShadowFBO);
    tex(m_dirShadowTex);
    smp(m_dirShadowCmpSampler);
    prog(m_dirShadowShader);
    // GBuffer
    fbo(m_gbufferFBO);
    tex(m_gbufferNormalTex);
    tex(m_gbufferMaterialTex);
    tex(m_gbufferAlbedoTex);
    tex(m_gbufferVelocityTex);
    tex(m_gbufferDepthTex);
    prog(m_gbufferShader);
    // SSAO / SSR / motion blur
    DeleteScreenSpace();
    prog(m_linearDepthShader);
    prog(m_ssaoShader);
    prog(m_ssaoBlurShader);
    prog(m_ssrShader);
    prog(m_ssrCopyShader);
    prog(m_ssrCompositeShader);
    prog(m_mbVelocityShader);
    prog(m_mbTileMaxShader);
    prog(m_mbNeighborMaxShader);
    prog(m_mbGatherShader);
    // FSR / NIS
    DeleteScaleTargets();
    prog(m_fsrEasuShader);
    prog(m_fsrRcasShader);
    DeleteNIS();
    // MSAA
    fbo(m_msaaFBO);
    tex(m_msaaColorTex);
    tex(m_msaaDepthTex);
    // HDR
    fbo(m_hdrFBO);
    tex(m_hdrColorTex);
    tex(m_hdrDepthTex);
    // Post-process shaders
    prog(m_tonemapShader);
    prog(m_bloomThreshShader);
    prog(m_bloomKawaseShader);
    prog(m_fxaaShader);
    prog(m_copyShader);
    // Bloom, LDR, AO fallback
    DeleteBloom();
    fbo(m_ldrFBO);
    tex(m_ldrTex);
    tex(m_whiteTex);
    // Quad
    if (m_quadVAO) glDeleteVertexArrays(1, &m_quadVAO);
    m_quadVAO = 0;
    buf(m_quadVBO);
}

void RenderSystem::DeleteBloom()
{
    glDeleteFramebuffers(1, &m_bloomThreshFBO); m_bloomThreshFBO = 0;
    glDeleteFramebuffers(1, &m_bloomPingFBO);   m_bloomPingFBO = 0;
    glDeleteFramebuffers(1, &m_bloomPongFBO);   m_bloomPongFBO = 0;
    glDeleteTextures(1, &m_bloomThreshTex);     m_bloomThreshTex = 0;
    glDeleteTextures(1, &m_bloomPingTex);       m_bloomPingTex = 0;
    glDeleteTextures(1, &m_bloomPongTex);       m_bloomPongTex = 0;
}

void RenderSystem::EnsureEffectTargets()
{
    const auto& rs = RenderSettings::instance();

    const bool bloom = rs.getBloomEnabled();
    if (bloom && !m_bloomThreshTex)      InitBloom();
    else if (!bloom && m_bloomThreshTex) DeleteBloom();

    // One set for the three: SSAO and SSR share the linear depth, and the motion blur gather reads it too.
    const bool screenSpace = rs.getSSAOEnabled() || rs.getSSREnabled() || rs.getMotionBlurEnabled();
    if (screenSpace && !m_linearDepthTex)      InitScreenSpace();
    else if (!screenSpace && m_linearDepthTex) DeleteScreenSpace();
}