#ifndef PARTICLE_SYSTEM_HPP
#define PARTICLE_SYSTEM_HPP

#include "ecs/ecs.hpp"
#include "ecs/ecs_common.hpp"
#include "ParticleEmitterComponent.hpp"
#include "OpenGL/OpenGLIncludes.hpp"
#include <glm/glm.hpp>
#include <random>
#include <vector>

// -------------------------------------------------------
//  ParticleSystem
//
//  ECS system that:
//    1. Simulates all ParticleEmitterComponents (CPU).
//    2. Uploads live particle data to a shared SSBO.
//    3. Issues instanced draw call(s) — one per distinct
//       blend mode — for all live particles.
//
//  Rendering is a billboard pass inserted into RenderSystem
//  after ShadingPass but before BloomPass so that emissive
//  particles feed through bloom automatically.
//
//  Usage:
//      particleSystem.Init();
//      // each frame, called by your engine loop:
//      particleSystem.Update(entityManager, events, false, dt);
//      // inside RenderSystem::Update, after ShadingPass:
//      particleSystem.Draw(view, projection);
// -------------------------------------------------------
class ParticleSystem : public ISystem {
public:
    // ---------------------------------------------------
    //  Call once after the OpenGL context is ready.
    // ---------------------------------------------------
    void Init();

    // ---------------------------------------------------
    //  Simulate all emitters and fill the staging buffer.
    //  Must be called before Draw().
    // ---------------------------------------------------
    void Update(EntityManager& entityManager,
        std::vector<EventEntry>& events,
        bool isServer,
        float deltaTime) override;

    // ---------------------------------------------------
    //  Upload staging data and issue billboard draw calls.
    //  Call from RenderSystem after ShadingPass while the
    //  HDR FBO is still bound.
    // ---------------------------------------------------
    void Draw(const glm::mat4& view, const glm::mat4& projection);

    ~ParticleSystem();

private:
    // =====================================================
    //  GPU resources
    // =====================================================
    GLuint m_shader = 0;
    GLuint m_quadVAO = 0;  // empty VAO — positions built in vert shader
    GLuint m_ssbo = 0;  // resized on demand, reused each frame
    int    m_ssboCapacity = 0;  // current GPUParticle capacity of m_ssbo

    // Cached uniform locations (set once in Init after shader compilation)
    GLint m_uView = -1;
    GLint m_uProjection = -1;

    // =====================================================
    //  Frame-local staging buffers, one per blend mode.
    //  Sorted so we issue at most two draw calls per frame.
    // =====================================================
    std::vector<GPUParticle> m_stagingAdditive;  // additiveBlend == true
    std::vector<GPUParticle> m_stagingAlpha;     // additiveBlend == false

    // =====================================================
    //  RNG — seeded once, shared across all emitters.
    //  Using mt19937 instead of std::rand() for quality,
    //  determinism (when desired), and thread-safety if
    //  the simulation ever moves to a worker thread.
    // =====================================================
    std::mt19937                          m_rng{ std::random_device{}() };
    std::uniform_real_distribution<float> m_dist{ 0.0f, 1.0f };

    float RandF() { return m_dist(m_rng); }

    // =====================================================
    //  Simulation helpers
    // =====================================================
    // Returns a spawn-offset in emitter-local space (before scale is applied).
    glm::vec3 SampleSpawnPosition(const ParticleEmitterComponent& e);

    // Returns a normalised spawn direction.
    // outConeT is set to the normalised [0,1] distance from the cone axis
    // (0 = centre, 1 = edge); only meaningful for EmitterShape::Cone.
    glm::vec3 SampleSpawnVelocity(const ParticleEmitterComponent& e,
        const glm::vec3& emitterWorldDir,
        float& outConeT);

    void SpawnParticle(ParticleEmitterComponent& e,
        const glm::vec3& emitterWorldPos,
        const glm::vec3& emitterWorldDir,
        float uniformScale);

    void SimulateEmitter(ParticleEmitterComponent& e,
        const glm::vec3& emitterWorldPos,
        const glm::vec3& emitterWorldDir,
        float uniformScale,
        float dt);

    // Initialise (or re-initialise) the pool and free-list to match
    // e.maxParticles. Safe to call multiple times.
    void EnsurePool(ParticleEmitterComponent& e);

    // =====================================================
    //  GPU helpers
    // =====================================================
    void EnsureSSBOCapacity(int needed);
    // Upload `src` to the SSBO and draw instanced with the given blend mode.
    void FlushStagingBuffer(std::vector<GPUParticle>& src,
        bool additive);
    void CompileShader();
    void InitQuadVAO();
};

#endif // PARTICLE_SYSTEM_HPP