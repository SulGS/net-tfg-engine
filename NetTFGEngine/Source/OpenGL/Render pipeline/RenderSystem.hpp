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
//                        MRT: attachment0=HDR color, attachment1=view-space normals+roughness
//    3. BloomPass      — threshold + Kawase blur
//    4. TonemapPass    — exposure + filmic/Reinhard + bloom composite + gamma → LDR FBO
//    5. FXAAPass       — anti-aliasing → default framebuffer
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

    // ---------------------------------------------------
    //  Debug: dump every pipeline buffer to "Render/<timestamp>/".
    //
    //  Files written (all PNG, 8-bit RGB):
    //    hdr_color.png          — HDR colour, auto-range normalised
    //    depth.png              — Scene depth, auto-ranged greyscale
    //    gbuffer_normal.png     — View-space normals as RGB (attachment 0)
    //    gbuffer_roughness.png  — Roughness as greyscale   (attachment 1)
    //    gbuffer_metalness.png  — Metalness as greyscale   (attachment 2)
    //    bloom_thresh.png       — Bloom threshold pass output
    //    bloom_result.png       — Final blurred bloom texture
    //    ldr_color.png          — Post-tonemap LDR colour (pre-FXAA)
    //    final_output.png       — Exact screen pixels (post-FXAA, pre-SwapBuffers)
    //    shadow_L{n}_F{f}.png   — Shadow cubemap face f of light n
    //
    //  A timestamped subfolder is created automatically.
    //  Safe to call at any time after Init().
    // ---------------------------------------------------
    void DumpBuffers() const;

    // ---------------------------------------------------
    //  Request a DumpBuffers() at the end of the next
    //  fully-completed frame.  Safe to call from any
    //  thread at any time — the flag is consumed once by
    //  Update() after FXAAPass() finishes, so every buffer
    //  is in its final state when the dump runs.
    //
    //  Typical use: bind to a key in your input handler.
    //    if (key == KEY_F9) renderSystem.RequestDebugDump();
    // ---------------------------------------------------
    void RequestDebugDump() { m_debugDumpRequested = true; }

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
    //  GBuffer framebuffer (geometry pre-pass)
    //  Renders view-space normals + roughness before shading.
    //  Shares the scene depth texture with the HDR FBO so the
    //  shading pass reads the same depth without a copy.
    // =====================================================
    GLuint m_gbufferFBO = 0;             // geometry pre-pass FBO
    GLuint m_gbufferNormalTex = 0;       // RGBA16F  attachment 0 — view-space normals (xyz) + roughness (w)
    GLuint m_gbufferRoughnessTex = 0;    // RGBA16F  attachment 1 — perceptual roughness (r)
    GLuint m_gbufferMetalnessTex = 0;    // RGBA16F  attachment 2 — metalness (r)
    GLuint m_gbufferDepthRBO = 0;        // DEPTH32F renderbuffer — owned exclusively by gbufferFBO

    // =====================================================
    //  MSAA framebuffer (shading renders here when samples > 1)
    //  Resolved into m_hdrFBO before post-processing.
    // =====================================================
    GLuint m_msaaFBO = 0;           // multisampled offscreen FBO
    GLuint m_msaaColorTex = 0;      // GL_TEXTURE_2D_MULTISAMPLE  RGBA16F
    GLuint m_msaaDepthTex = 0;      // GL_TEXTURE_2D_MULTISAMPLE  DEPTH32F
    int    m_msaaSamples = 1;       // actual sample count in use (1 = disabled)

    // =====================================================
    //  HDR framebuffer + screen-quad resources
    // =====================================================
    GLuint m_hdrFBO = 0;        // resolve target / post-process source
    GLuint m_hdrColorTex = 0;   // attachment0: RGBA16F HDR color
    GLuint m_hdrDepthTex = 0;   // depth attachment (shared with m_gbufferFBO)
    GLuint m_quadVAO = 0;       // screen-space triangle VAO
    GLuint m_quadVBO = 0;

    // =====================================================
    //  Debug state
    // =====================================================
    mutable std::atomic<bool> m_debugDumpRequested{ false };

    // =====================================================
    //  Bloom resources
    // =====================================================
    GLuint m_bloomThreshFBO = 0; // FBO for the threshold pass
    GLuint m_bloomThreshTex = 0; // RGBA16F — pixels above threshold
    GLuint m_bloomPingFBO = 0; // Kawase ping buffer
    GLuint m_bloomPingTex = 0;
    GLuint m_bloomPongFBO = 0; // Kawase pong buffer
    GLuint m_bloomPongTex = 0;

    // =====================================================
    //  LDR framebuffer (tonemap output, FXAA input)
    // =====================================================
    GLuint m_ldrFBO = 0;  // tonemap writes here
    GLuint m_ldrTex = 0;  // RGBA8

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
    void InitGBufferFBO();           // creates gbuffer normal+roughness texture
    void InitMSAAFBO();              // creates multisampled FBO (no-op when samples == 1)
    void InitHDRFBO();
    void InitBloom();
    void InitLDRFBO();
    void InitScreenQuad();

    // =====================================================
    //  MSAA resolve helper
    // =====================================================
    void ResolveMSAA();          // blits m_msaaFBO → m_hdrFBO; no-op when samples == 1

    // =====================================================
    //  Shader compilation helpers
    // =====================================================
    static GLuint CompileStage(GLenum type, const char* src);
    static GLuint LinkProgram(std::initializer_list<GLuint> stages);

    void CompileGBufferShader();
    void CompileShadowShader();
    void CompileTonemapShader();
    void CompileBloomShaders();
    void CompileFXAAShader();

    // =====================================================
    //  Per-frame passes
    // =====================================================
    void GBufferPass(EntityManager::Query<MeshComponent, Transform>& meshQuery, const glm::mat4& view,
        const glm::mat4& projection);
    void ShadowPass(EntityManager& em, EntityManager::Query<MeshComponent, Transform>& meshQuery);
    void ShadingPass(EntityManager::Query<MeshComponent, Transform>& meshQuery, const glm::mat4& view,
        const glm::mat4& projection, const glm::vec3& cameraPos);
    void BloomPass();
    void TonemapPass();
    void FXAAPass();
};

#endif // RENDER_SYSTEM_HPP