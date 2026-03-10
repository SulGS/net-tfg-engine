#include "RenderSystem.hpp"

void RenderSystem::Init(int screenW, int screenH)
{
    const auto& rs = RenderSettings::instance();
    MAX_LIGHTS = rs.getMaxLights();
    MAX_LIGHTS = rs.getMaxLights();
    MAX_SHADOW_LIGHTS = rs.getMaxShadowLights();

    m_screenW = screenW;
    m_screenH = screenH;

    InitHDRFBO();
    InitLightSSBO();
    InitLightSSBO();
    InitShadowCubeArray();
    InitBloom();
    InitBloomBlackTex();
    InitFXAA();
    CompileShadowShader();
    CompileTonemapShader();
    CompileBloomShaders();
    CompileFXAAShader();
    InitScreenQuad();
}
void RenderSystem::Resize(int screenW, int screenH)
{
    m_screenW = screenW;
    m_screenH = screenH;

    glDeleteFramebuffers(1, &m_hdrFBO);  m_hdrFBO = 0;
    glDeleteTextures(1, &m_hdrColorTex); m_hdrColorTex = 0;
    glDeleteTextures(1, &m_hdrDepthTex); m_hdrDepthTex = 0;
    InitHDRFBO();

    glDeleteFramebuffers(1, &m_bloomThreshFBO); m_bloomThreshFBO = 0;
    glDeleteFramebuffers(1, &m_bloomPingFBO);   m_bloomPingFBO = 0;
    glDeleteFramebuffers(1, &m_bloomPongFBO);   m_bloomPongFBO = 0;
    glDeleteTextures(1, &m_bloomThreshTex);     m_bloomThreshTex = 0;
    glDeleteTextures(1, &m_bloomPingTex);       m_bloomPingTex = 0;
    glDeleteTextures(1, &m_bloomPongTex);       m_bloomPongTex = 0;
    InitBloom();

    glDeleteFramebuffers(1, &m_fxaaFBO); m_fxaaFBO = 0;
    glDeleteTextures(1, &m_fxaaTex);     m_fxaaTex = 0;
    InitFXAA();
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

    if (RenderSettings::instance().getShadowsEnabled())
        ShadowPass(entityManager, meshQuery);
    else
        m_shadowCount = 0;

    ShadingPass(meshQuery, view, projection, cameraPos);

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
    glDeleteBuffers(1, &m_lightSSBO);
    glDeleteBuffers(1, &m_lightSSBO);
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
    glDeleteFramebuffers(1, &m_bloomThreshFBO);
    glDeleteFramebuffers(1, &m_bloomPingFBO);
    glDeleteFramebuffers(1, &m_bloomPongFBO);
    glDeleteTextures(1, &m_bloomThreshTex);
    glDeleteTextures(1, &m_bloomPingTex);
    glDeleteTextures(1, &m_bloomPongTex);
    glDeleteProgram(m_bloomThreshShader);
    glDeleteProgram(m_bloomKawaseShader);
    glDeleteTextures(1, &m_bloomBlackTex);
    glDeleteFramebuffers(1, &m_fxaaFBO);
    glDeleteTextures(1, &m_fxaaTex);
    glDeleteProgram(m_fxaaShader);
}