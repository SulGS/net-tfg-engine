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
#include <filesystem>
#include <atomic>

#include "OpenGL/Particles/ParticleSystem.hpp"


struct GPUPointLight {
    glm::vec4 posRadius;      // xyz = world pos,  w = radius
    glm::vec4 colorIntensity; // rgb = color,       a = intensity
};

// -------------------------------------------------------
//  GPU layout for the directional light uniform block.
//  Sent via a UBO (binding 2).  Padded to std140 rules.
// -------------------------------------------------------
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

// -------------------------------------------------------
//  Concrete type for the mesh query used across passes.
// -------------------------------------------------------
using MeshQuery = decltype(
    std::declval<EntityManager>().CreateQuery<MeshComponent, Transform>());

// -------------------------------------------------------
//  RenderSystem
//
//  Per-frame pipeline:
//    1. GBufferPass    — view-space normals + roughness + metalness
//    2. CollectLights  — upload point lights + directional light to GPU
//    3. ShadowPass     — depth cubemap array for shadow-casting point lights
//    4. DirShadowPass  — single ortho shadow map for the directional light
//    5. ShadingPass    — full mesh draw with shadow lookup
//    6. BloomPass      — threshold + Kawase blur
//    7. TonemapPass    — exposure + filmic/Reinhard + bloom composite + gamma → LDR FBO
//    8. FXAAPass       — anti-aliasing → default framebuffer
// -------------------------------------------------------
class RenderSystem : public ISystem {
public:

    void Init(int screenW, int screenH);
    void Resize(int screenW, int screenH);
    void ReInitShadows();

    void Update(EntityManager& entityManager,
        std::vector<EventEntry>& events,
        bool isServer,
        float deltaTime) override;

    ~RenderSystem();

    void DumpBuffers() const;
    void RequestDebugDump() { m_debugDumpRequested = true; }

    void SetParticleSystem(ParticleSystem* ps) { m_particleSystem = ps; }

private:
    // =====================================================
    //  Init-time constants
    // =====================================================
    int MAX_LIGHTS = 512;
    int MAX_SHADOW_LIGHTS = 8;

    // =====================================================
    //  Screen / misc
    // =====================================================
    int m_screenW = 0, m_screenH = 0;
    int m_lightCount = 0;

    ParticleSystem* m_particleSystem = nullptr;

    // =====================================================
    //  Point light SSBO  (binding 0)
    // =====================================================
    GLuint m_lightSSBO = 0;

    // =====================================================
    //  Point light shadow resources
    // =====================================================
    GLuint m_shadowCubeArray = 0;
    GLuint m_shadowFBO = 0;
    GLuint m_shadowShader = 0;
    GLuint m_shadowDataSSBO = 0;
    int    m_shadowRes = 512;
    int    m_shadowCount = 0;

    // =====================================================
    //  Directional light UBO  (binding 2)
    //  One GPUDirLight struct.  colorEnabled.a == 0 means
    //  "no directional light" — the shader skips the term.
    // =====================================================
    GLuint m_dirLightUBO = 0;

    // =====================================================
    //  Directional light shadow map
    //  A single DEPTH32F texture + dedicated FBO.
    //  Resolution is controlled by getShadowResolution()
    //  (same setting as point lights for simplicity).
    // =====================================================
    GLuint m_dirShadowTex = 0;   // sampler2D, DEPTH32F
    GLuint m_dirShadowFBO = 0;
    GLuint m_dirShadowShader = 0;

    // =====================================================
    //  GBuffer framebuffer
    // =====================================================
    GLuint m_gbufferFBO = 0;
    GLuint m_gbufferNormalTex = 0;
    GLuint m_gbufferRoughnessTex = 0;
    GLuint m_gbufferMetalnessTex = 0;
    GLuint m_gbufferDepthRBO = 0;

    // =====================================================
    //  MSAA framebuffer
    // =====================================================
    GLuint m_msaaFBO = 0;
    GLuint m_msaaColorTex = 0;
    GLuint m_msaaDepthTex = 0;
    int    m_msaaSamples = 1;

    // =====================================================
    //  HDR framebuffer + screen-quad
    // =====================================================
    GLuint m_hdrFBO = 0;
    GLuint m_hdrColorTex = 0;
    GLuint m_hdrDepthTex = 0;
    GLuint m_quadVAO = 0;
    GLuint m_quadVBO = 0;

    // =====================================================
    //  Debug state
    // =====================================================
    mutable std::atomic<bool> m_debugDumpRequested{ false };

    // =====================================================
    //  Bloom resources
    // =====================================================
    GLuint m_bloomThreshFBO = 0;
    GLuint m_bloomThreshTex = 0;
    GLuint m_bloomPingFBO = 0;
    GLuint m_bloomPingTex = 0;
    GLuint m_bloomPongFBO = 0;
    GLuint m_bloomPongTex = 0;

    // =====================================================
    //  LDR framebuffer
    // =====================================================
    GLuint m_ldrFBO = 0;
    GLuint m_ldrTex = 0;

    // =====================================================
    //  Shader programs
    // =====================================================
    GLuint m_gbufferShader = 0;
    GLuint m_tonemapShader = 0;
    GLuint m_bloomThreshShader = 0;
    GLuint m_bloomKawaseShader = 0;
    GLuint m_fxaaShader = 0;

    // =====================================================
    //  Initialisation helpers
    // =====================================================
    void InitLightSSBO();
    void InitShadowCubeArray();
    void InitDirLightUBO();        // NEW
    void InitDirShadowMap();       // NEW
    void InitGBufferFBO();
    void InitMSAAFBO();
    void InitHDRFBO();
    void InitBloom();
    void InitLDRFBO();
    void InitScreenQuad();

    void ResolveMSAA();

    // =====================================================
    //  Shader compilation helpers
    // =====================================================
    static GLuint CompileStage(GLenum type, const char* src);
    static GLuint LinkProgram(std::initializer_list<GLuint> stages);

    void CompileGBufferShader();
    void CompileShadowShader();
    void CompileDirShadowShader(); // NEW
    void CompileTonemapShader();
    void CompileBloomShaders();
    void CompileFXAAShader();

    // =====================================================
    //  Per-frame passes
    // =====================================================
    void GBufferPass(EntityManager::Query<MeshComponent, Transform>& meshQuery,
        const glm::mat4& view, const glm::mat4& projection);
    void CollectLightsPass(EntityManager& em);  // also handles directional light
    void ShadowPass(EntityManager& em, EntityManager::Query<MeshComponent, Transform>& meshQuery);
    void DirShadowPass(EntityManager::Query<MeshComponent, Transform>& meshQuery); // NEW
    void ShadingPass(EntityManager::Query<MeshComponent, Transform>& meshQuery,
        const glm::mat4& view, const glm::mat4& projection,
        const glm::vec3& cameraPos);
    void BloomPass();
    void TonemapPass();
    void FXAAPass();
};

#endif // RENDER_SYSTEM_HPP