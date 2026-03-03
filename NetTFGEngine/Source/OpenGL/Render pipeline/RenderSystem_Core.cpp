#include "RenderSystem.hpp"

void RenderSystem::Init(int screenW, int screenH)
{
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

void RenderSystem::Resize(int screenW, int screenH)
{
    m_screenW = screenW;
    m_screenH = screenH;
    m_tilesX = (screenW + TILE_SIZE - 1) / TILE_SIZE;
    m_tilesY = (screenH + TILE_SIZE - 1) / TILE_SIZE;

    glDeleteFramebuffers(1, &m_depthFBO); m_depthFBO = 0;
    glDeleteTextures(1, &m_depthTex);     m_depthTex = 0;
    InitDepthFBO();

    glDeleteFramebuffers(1, &m_hdrFBO);  m_hdrFBO = 0;
    glDeleteTextures(1, &m_hdrColorTex); m_hdrColorTex = 0;
    glDeleteTextures(1, &m_hdrDepthTex); m_hdrDepthTex = 0;
    InitHDRFBO();

    glDeleteFramebuffers(1, &m_ssaoFBO);     m_ssaoFBO = 0;
    glDeleteFramebuffers(1, &m_ssaoBlurFBO); m_ssaoBlurFBO = 0;
    glDeleteTextures(1, &m_ssaoTex);         m_ssaoTex = 0;
    glDeleteTextures(1, &m_ssaoBlurTex);     m_ssaoBlurTex = 0;
    InitSSAO();

    glDeleteFramebuffers(1, &m_bloomThreshFBO); m_bloomThreshFBO = 0;
    glDeleteFramebuffers(1, &m_bloomPingFBO);   m_bloomPingFBO = 0;
    glDeleteFramebuffers(1, &m_bloomPongFBO);   m_bloomPongFBO = 0;
    glDeleteTextures(1, &m_bloomThreshTex);     m_bloomThreshTex = 0;
    glDeleteTextures(1, &m_bloomPingTex);       m_bloomPingTex = 0;
    glDeleteTextures(1, &m_bloomPongTex);       m_bloomPongTex = 0;
    InitBloom();

    glDeleteFramebuffers(1, &m_ssrFBO); m_ssrFBO = 0;
    glDeleteTextures(1, &m_ssrTex);     m_ssrTex = 0;
    InitSSR();

    glDeleteFramebuffers(1, &m_fxaaFBO); m_fxaaFBO = 0;
    glDeleteTextures(1, &m_fxaaTex);     m_fxaaTex = 0;
    InitFXAA();

    // m_lightSSBO is fixed MAX_LIGHTS size -- do NOT touch it
    int totalTiles = m_tilesX * m_tilesY;

    glDeleteBuffers(1, &m_lightIndexSSBO); m_lightIndexSSBO = 0;
    glGenBuffers(1, &m_lightIndexSSBO);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, m_lightIndexSSBO);
    glBufferData(GL_SHADER_STORAGE_BUFFER,
        sizeof(uint32_t) * totalTiles * MAX_LIGHTS_TILE, nullptr, GL_DYNAMIC_DRAW);

    glDeleteBuffers(1, &m_tileGridSSBO); m_tileGridSSBO = 0;
    glGenBuffers(1, &m_tileGridSSBO);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, m_tileGridSSBO);
    glBufferData(GL_SHADER_STORAGE_BUFFER,
        sizeof(GPUTileData) * totalTiles, nullptr, GL_DYNAMIC_DRAW);

    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
}

void RenderSystem::ReInitShadows()
{
    glDeleteFramebuffers(1, &m_shadowFBO);   m_shadowFBO = 0;
    glDeleteTextures(1, &m_shadowCubeArray); m_shadowCubeArray = 0;
    glDeleteBuffers(1, &m_shadowDataSSBO);   m_shadowDataSSBO = 0;
    InitShadowCubeArray();
}

void RenderSystem::Update(EntityManager& entityManager,
    std::vector<EventEntry>& events,
    bool /*isServer*/,
    float /*deltaTime*/)
{
    if (m_depthFBO == 0) {
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

    UploadLights(entityManager);

    if (RenderSettings::instance().getShadowsEnabled())
        ShadowPass(entityManager, meshQuery);
    else
        m_shadowCount = 0;

	DepthPrePass(meshQuery, view, projection);

    LightCullPass(view, projection);

	ShadingPass(meshQuery, view, projection, cameraPos);

    if (RenderSettings::instance().getSSAOEnabled())
        SSAOPass(projection);

    if (RenderSettings::instance().getSSREnabled())
        SSRPass(view, projection);

    if (RenderSettings::instance().getBloomEnabled())
        BloomPass();

    TonemapPass();

    if (RenderSettings::instance().getFXAAEnabled())
        FXAAPass();
    else
        BlitLDRToScreen();

    entityManager.releaseMutex();
}

RenderSystem::~RenderSystem()
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
    glDeleteFramebuffers(1, &m_ssaoFBO);
    glDeleteFramebuffers(1, &m_ssaoBlurFBO);
    glDeleteTextures(1, &m_ssaoTex);
    glDeleteTextures(1, &m_ssaoBlurTex);
    glDeleteTextures(1, &m_ssaoNoiseTex);
    glDeleteProgram(m_ssaoShader);
    glDeleteProgram(m_ssaoBlurShader);
    glDeleteFramebuffers(1, &m_bloomThreshFBO);
    glDeleteFramebuffers(1, &m_bloomPingFBO);
    glDeleteFramebuffers(1, &m_bloomPongFBO);
    glDeleteTextures(1, &m_bloomThreshTex);
    glDeleteTextures(1, &m_bloomPingTex);
    glDeleteTextures(1, &m_bloomPongTex);
    glDeleteProgram(m_bloomThreshShader);
    glDeleteProgram(m_bloomKawaseShader);
    glDeleteFramebuffers(1, &m_ssrFBO);
    glDeleteTextures(1, &m_ssrTex);
    glDeleteProgram(m_ssrShader);
    glDeleteFramebuffers(1, &m_fxaaFBO);
    glDeleteTextures(1, &m_fxaaTex);
    glDeleteProgram(m_fxaaShader);
}