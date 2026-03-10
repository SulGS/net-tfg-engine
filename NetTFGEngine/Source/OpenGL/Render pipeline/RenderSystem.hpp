#ifndef RENDER_SYSTEM_HPP
#define RENDER_SYSTEM_HPP

#include "ecs/ecs.hpp"
#include "ecs/ecs_common.hpp"
#include "Utils/Debug/Debug.hpp"
#include "OpenGL/Mesh.hpp"
#include "OpenGl/Material.hpp"
#include "OpenGl/OpenGLIncludes.hpp"
#include "RenderSettings.hpp"
#include <glm/gtc/type_ptr.hpp>
#include <vector>
#include <string>
#include <iostream>
#include <cstdlib>
#include <cmath>
#include <algorithm>



struct GPUPointLight {
    glm::vec4 posRadius;      // xyz = world pos,  w = radius
    glm::vec4 colorIntensity; // rgb = color,       a = intensity
};

struct GPUShadowData {
    glm::mat4 lightSpaceMatrices[6]; // one per cube face
    int       lightIndex;
    float     farPlane;
    int       pad[2];
};

// -------------------------------------------------------
//  Concrete type for the mesh query used across passes.
//  Derived from EntityManager::CreateQuery so every .cpp
//  can name the type without repeating the template args.
// -------------------------------------------------------
using MeshQuery = decltype(
    std::declval<EntityManager>().CreateQuery<MeshComponent, Transform>());

// -------------------------------------------------------
//  RenderSystem
//
//  Per-frame pipeline:
//    1. ShadowPass     — render depth cubemap array for
//                        all shadow-casting point lights
//    2. ShadingPass    — full mesh draw with shadow lookup
//    3. BloomPass      — threshold + Kawase blur (optional)
//    4. TonemapPass    — exposure + filmic/Reinhard + gamma
//    5. FXAAPass       — edge-smoothing on LDR output
// -------------------------------------------------------
class RenderSystem : public ISystem {
public:

    // ---------------------------------------------------
    //  Call once after the OpenGL context is ready.
    //  Reads init-time constants from RenderSettings.
    //  screenW / screenH must match your actual viewport.
    // ---------------------------------------------------
    void Init(int screenW, int screenH);

    // ---------------------------------------------------
    //  Call when the window is resized.
    // ---------------------------------------------------
    void Resize(int screenW, int screenH);

    // ---------------------------------------------------
    //  Re-creates the shadow cubemap array using the
    //  current RenderSettings::getShadowResolution().
    //  Call this after changing shadow resolution at runtime.
    // ---------------------------------------------------
    void ReInitShadows();

    void Update(EntityManager& entityManager,
        std::vector<EventEntry>& events,
        bool isServer,
        float deltaTime) override;

    ~RenderSystem();

private:
    // =====================================================
    //  Init-time constants — set from RenderSettings in Init()
    // =====================================================
    int MAX_LIGHTS = 512;
    int MAX_SHADOW_LIGHTS = 8;

    // =====================================================
    //  GPU resource handles
    // =====================================================
    int m_screenW = 0, m_screenH = 0;
    int m_lightCount = 0;

    GLuint m_lightSSBO = 0; // binding 0 — GPUPointLight array

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
    GLuint m_hdrDepthTex = 0;  // shared depth attachment
    GLuint m_tonemapShader = 0;  // fullscreen tonemap + gamma program
    GLuint m_quadVAO = 0;  // screen-space triangle VAO
    GLuint m_quadVBO = 0;

    // =====================================================
    //  Bloom resources
    // =====================================================
    GLuint m_bloomThreshFBO = 0;
    GLuint m_bloomThreshTex = 0;  // RGBA16F -- bright pixels only
    GLuint m_bloomPingFBO = 0;
    GLuint m_bloomPingTex = 0;  // RGBA16F -- ping buffer (half-res)
    GLuint m_bloomPongFBO = 0;
    GLuint m_bloomPongTex = 0;  // RGBA16F -- pong buffer (half-res)
    GLuint m_bloomThreshShader = 0;
    GLuint m_bloomKawaseShader = 0;
    GLuint m_bloomResultTex = 0;  // points to ping or pong after BloomPass
    GLuint m_bloomBlackTex = 0;  // 1x1 black fallback when bloom is disabled

    // =====================================================
    //  FXAA resources
    // =====================================================
    GLuint m_fxaaFBO = 0;  // LDR intermediate (RGBA8)
    GLuint m_fxaaTex = 0;  // RGBA8 -- tonemapped LDR
    GLuint m_fxaaShader = 0;

    // =====================================================
    //  Initialisation helpers
    // =====================================================
    void InitLightSSBO();
    void InitShadowCubeArray();
    void InitHDRFBO();
    void InitScreenQuad();
    void InitBloom();
    void InitFXAA();
    void InitBloomBlackTex();

    // =====================================================
    //  Shader compilation helpers
    // =====================================================
    static GLuint CompileStage(GLenum type, const char* src);
    static GLuint LinkProgram(std::initializer_list<GLuint> stages);

    void CompileShadowShader();
    void CompileTonemapShader();
    void CompileBloomShaders();
    void CompileFXAAShader();

    // =====================================================
    //  Per-frame passes
    // =====================================================
    void ShadowPass(EntityManager& em, EntityManager::Query<MeshComponent, Transform>& meshQuery);
    void ShadingPass(EntityManager::Query<MeshComponent, Transform>& meshQuery, const glm::mat4& view,
        const glm::mat4& projection, const glm::vec3& cameraPos);
    void BloomPass();
    void TonemapPass();
    void FXAAPass();
    void BlitLDRToScreen();
};

#endif // RENDER_SYSTEM_HPP