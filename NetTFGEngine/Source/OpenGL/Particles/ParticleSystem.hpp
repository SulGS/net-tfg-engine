#ifndef PARTICLE_SYSTEM_HPP
#define PARTICLE_SYSTEM_HPP

#include "ecs/ecs.hpp"
#include "ecs/ecs_common.hpp"
#include "ParticleEmitterComponent.hpp"
#include "OpenGl/OpenGLIncludes.hpp"
#include <glm/glm.hpp>
#include <vector>

// -------------------------------------------------------
//  ParticleSystem
//
//  ECS system that:
//    1. Simulates all ParticleEmitterComponents (CPU).
//    2. Uploads live particle data to a per-emitter SSBO.
//    3. Issues one instanced draw call per emitter.
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
    //  Simulate + upload to GPU.  Must be called before Draw().
    // ---------------------------------------------------
    void Update(EntityManager& entityManager,
        std::vector<EventEntry>& events,
        bool isServer,
        float deltaTime) override;

    // ---------------------------------------------------
    //  Issue billboard draw calls for all live emitters.
    //  Call from RenderSystem after ShadingPass, while the
    //  HDR FBO is still bound.
    // ---------------------------------------------------
    void Draw(const glm::mat4& view, const glm::mat4& projection);

    ~ParticleSystem();

private:
    // =====================================================
    //  GPU resources shared across all emitters
    // =====================================================
    GLuint m_shader = 0;
    GLuint m_quadVAO = 0;   // empty VAO — vertex positions built in vert shader
    GLuint m_ssbo = 0;   // re-used each frame; resized on demand
    int    m_ssboCapacity = 0; // current GPUParticle capacity of m_ssbo

    // =====================================================
    //  Frame-local staging buffer
    // =====================================================
    std::vector<GPUParticle> m_staging;

    // =====================================================
    //  Simulation helpers
    // =====================================================
    static glm::vec3 SampleSpawnPosition(const ParticleEmitterComponent& e,
        const glm::vec3& emitterWorldPos);

    static glm::vec3 SampleSpawnVelocity(const ParticleEmitterComponent& e,
        const glm::vec3& emitterWorldDir);

    void SpawnParticle(ParticleEmitterComponent& e,
        const glm::vec3& emitterWorldPos,
        const glm::vec3& emitterWorldDir,
        float uniformScale);

    void SimulateEmitter(ParticleEmitterComponent& e,
        const glm::vec3& emitterWorldPos,
        const glm::vec3& emitterWorldDir,
        float uniformScale,
        float dt);

    // =====================================================
    //  GPU helpers
    // =====================================================
    void EnsureSSBOCapacity(int needed);
    void CompileShader();
    void InitQuadVAO();
};

#endif // PARTICLE_SYSTEM_HPP