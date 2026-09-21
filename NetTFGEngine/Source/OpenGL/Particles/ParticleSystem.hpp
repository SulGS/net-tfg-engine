#ifndef PARTICLE_SYSTEM_HPP
#define PARTICLE_SYSTEM_HPP

#include "ecs/ecs.hpp"
#include "ecs/ecs_common.hpp"
#include "ParticleEmitterComponent.hpp"
#include "OpenGL/OpenGLIncludes.hpp"
#include <glm/glm.hpp>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

// ECS system: simulates ParticleEmitterComponents on the CPU, uploads live particles to a shared SSBO, and issues one instanced draw call per batch (texture x blend mode). Draw() runs inside RenderSystem after ShadingPass but before BloomPass so emissive particles feed through bloom.
class ParticleSystem : public ISystem {
public:
    // Call once after the OpenGL context is ready.
    void Init();

    // Simulate all emitters and fill the staging buffers; must be called before Draw().
    void Update(EntityManager& entityManager,
        std::vector<EventEntry>& events,
        bool isServer,
        float deltaTime) override;

    // Upload staging data and issue billboard draw calls; call from RenderSystem after ShadingPass while the HDR FBO is still bound. Order: distortion, alpha-blended, additive. Distortion batches refract a copy of the framebuffer taken at the start of this call, so it must contain the finished scene.
    void Draw(const glm::mat4& view, const glm::mat4& projection);

    ~ParticleSystem();

private:
    // Everything that forces a separate draw call: which sheet is bound, how it is combined, and which blend/shader is used.
    struct BatchKey {
        GLuint tex = 0;          // 0 = procedural disc
        int    alphaMode = 0;    // 0 procedural, else (FlipbookAlpha + 1)
        bool   additive = true;
        bool   distortion = false;

        bool operator==(const BatchKey& o) const {
            return tex == o.tex && alphaMode == o.alphaMode
                && additive == o.additive && distortion == o.distortion;
        }
    };
    struct Batch {
        BatchKey                 key;
        std::vector<GPUParticle> data;   // rebuilt every Update
    };

    GLuint m_shader = 0;       // colour sprites (procedural / flipbook)
    GLuint m_distShader = 0;   // screen-space distortion sprites
    GLuint m_quadVAO = 0;      // empty VAO — positions built in vert shader
    GLuint m_ssbo = 0;         // resized on demand, reused each frame
    int    m_ssboCapacity = 0; // current GPUParticle capacity of m_ssbo

    // Cached uniform locations (set once in Init after shader compilation)
    GLint m_uView = -1, m_uProjection = -1;
    GLint m_uTex = -1, m_uAlphaMode = -1;
    GLint m_dView = -1, m_dProjection = -1, m_dScene = -1, m_dViewport = -1;

    std::vector<Batch> m_batches;

    // Sprite sheets requested by emitters, loaded lazily through the AssetManager. 0 = load failed (emitter falls back to the procedural disc).
    std::unordered_map<std::string, GLuint> m_textures;

    // Copy of the HDR scene used as the refraction source by distortion batches.
    GLuint m_sceneTex = 0;
    int    m_sceneW = 0, m_sceneH = 0;

    // RNG — seeded once and shared across emitters; mt19937 for quality/determinism, ready for a future worker thread.
    std::mt19937                          m_rng{ std::random_device{}() };
    std::uniform_real_distribution<float> m_dist{ 0.0f, 1.0f };

    float RandF() { return m_dist(m_rng); }

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

    Batch& GetBatch(const BatchKey& key);
    GLuint GetTexture(const std::string& name);
    void   CopySceneColor(int w, int h);

    void EnsureSSBOCapacity(int needed);
    // Upload the batch to the SSBO and draw it instanced (blend function must already be set).
    void FlushBatch(const Batch& batch);
    void CompileShaders();
    void InitQuadVAO();
};

#endif // PARTICLE_SYSTEM_HPP
