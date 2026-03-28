#include "RenderSystem.hpp"

void RenderSystem::Init(int screenW, int screenH)
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

    glEnable(GL_TEXTURE_CUBE_MAP_SEAMLESS);

    InitMSAAFBO();
    InitHDRFBO();
    InitGBufferFBO();
    InitBloom();
    InitLDRFBO();
    InitLightSSBO();
    InitShadowCubeArray();
    InitDirLightUBO();       // NEW
    InitDirShadowMap();      // NEW
    CompileGBufferShader();
    CompileShadowShader();
    CompileDirShadowShader(); // NEW
    CompileTonemapShader();
    CompileBloomShaders();
    CompileFXAAShader();
    InitScreenQuad();
}

void RenderSystem::Resize(int screenW, int screenH)
{
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
    glDeleteTextures(1, &m_gbufferRoughnessTex); m_gbufferRoughnessTex = 0;
    glDeleteTextures(1, &m_gbufferMetalnessTex); m_gbufferMetalnessTex = 0;
    glDeleteRenderbuffers(1, &m_gbufferDepthRBO); m_gbufferDepthRBO = 0;
    InitGBufferFBO();

    glDeleteFramebuffers(1, &m_bloomThreshFBO); m_bloomThreshFBO = 0;
    glDeleteFramebuffers(1, &m_bloomPingFBO);   m_bloomPingFBO = 0;
    glDeleteFramebuffers(1, &m_bloomPongFBO);   m_bloomPongFBO = 0;
    glDeleteTextures(1, &m_bloomThreshTex);     m_bloomThreshTex = 0;
    glDeleteTextures(1, &m_bloomPingTex);       m_bloomPingTex = 0;
    glDeleteTextures(1, &m_bloomPongTex);       m_bloomPongTex = 0;
    InitBloom();

    glDeleteFramebuffers(1, &m_ldrFBO); m_ldrFBO = 0;
    glDeleteTextures(1, &m_ldrTex);     m_ldrTex = 0;
    InitLDRFBO();

    // Dir shadow map resolution does not depend on screen size — no resize needed.
}

void RenderSystem::ReInitShadows()
{
    // Point light shadows
    glDeleteFramebuffers(1, &m_shadowFBO);   m_shadowFBO = 0;
    glDeleteTextures(1, &m_shadowCubeArray); m_shadowCubeArray = 0;
    glDeleteBuffers(1, &m_shadowDataSSBO);   m_shadowDataSSBO = 0;
    InitShadowCubeArray();

    // Directional light shadow (same resolution setting)
    glDeleteFramebuffers(1, &m_dirShadowFBO); m_dirShadowFBO = 0;
    glDeleteTextures(1, &m_dirShadowTex);     m_dirShadowTex = 0;
    InitDirShadowMap();
}

void RenderSystem::Update(EntityManager& entityManager,
    std::vector<EventEntry>& events,
    bool /*isServer*/,
    float /*deltaTime*/)
{
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
        entityManager.releaseMutex();
        return;
    }

    glm::mat4 view = activeCamera->getViewMatrix();
    glm::mat4 projection = activeCamera->getProjectionMatrix();
    glm::vec3 cameraPos = cameraTransform->getPosition();

    auto meshQuery = entityManager.CreateQuery<MeshComponent, Transform>();

    GBufferPass(meshQuery, view, projection);

    // CollectLightsPass now handles both point lights and the directional light.
    CollectLightsPass(entityManager);

    const auto& rs = RenderSettings::instance();

    if (rs.getShadowsEnabled())
        ShadowPass(entityManager, meshQuery);   // point light cubemap shadows
    else
        m_shadowCount = 0;

    if (rs.getDirShadowsEnabled())
        DirShadowPass(meshQuery);               // directional light ortho shadow

    ShadingPass(meshQuery, view, projection, cameraPos);

    if (m_particleSystem)
        m_particleSystem->Draw(view, projection);

    if (RenderSettings::instance().getBloomEnabled())
        BloomPass();

    TonemapPass();
    FXAAPass();

    entityManager.releaseMutex();
}

RenderSystem::~RenderSystem()
{
    glDeleteBuffers(1, &m_lightSSBO);
    glDeleteFramebuffers(1, &m_shadowFBO);
    glDeleteTextures(1, &m_shadowCubeArray);
    glDeleteBuffers(1, &m_shadowDataSSBO);
    glDeleteProgram(m_shadowShader);
    // Directional light
    glDeleteBuffers(1, &m_dirLightUBO);
    glDeleteFramebuffers(1, &m_dirShadowFBO);
    glDeleteTextures(1, &m_dirShadowTex);
    glDeleteProgram(m_dirShadowShader);
    // GBuffer
    glDeleteFramebuffers(1, &m_gbufferFBO);
    glDeleteTextures(1, &m_gbufferNormalTex);
    glDeleteTextures(1, &m_gbufferRoughnessTex);
    glDeleteTextures(1, &m_gbufferMetalnessTex);
    glDeleteRenderbuffers(1, &m_gbufferDepthRBO);
    glDeleteProgram(m_gbufferShader);
    // MSAA
    glDeleteFramebuffers(1, &m_msaaFBO);
    glDeleteTextures(1, &m_msaaColorTex);
    glDeleteTextures(1, &m_msaaDepthTex);
    // HDR
    glDeleteFramebuffers(1, &m_hdrFBO);
    glDeleteTextures(1, &m_hdrColorTex);
    glDeleteTextures(1, &m_hdrDepthTex);
    // Post-process shaders
    glDeleteProgram(m_tonemapShader);
    glDeleteProgram(m_bloomThreshShader);
    glDeleteProgram(m_bloomKawaseShader);
    glDeleteProgram(m_fxaaShader);
    // Bloom
    glDeleteFramebuffers(1, &m_bloomThreshFBO);
    glDeleteFramebuffers(1, &m_bloomPingFBO);
    glDeleteFramebuffers(1, &m_bloomPongFBO);
    glDeleteTextures(1, &m_bloomThreshTex);
    glDeleteTextures(1, &m_bloomPingTex);
    glDeleteTextures(1, &m_bloomPongTex);
    // LDR
    glDeleteFramebuffers(1, &m_ldrFBO);
    glDeleteTextures(1, &m_ldrTex);
    // Quad
    glDeleteVertexArrays(1, &m_quadVAO);
    glDeleteBuffers(1, &m_quadVBO);
}