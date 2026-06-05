#include "ParticleSystem.hpp"
#include "Utils/Debug/Debug.hpp"
#include <glm/gtc/type_ptr.hpp>
#include <algorithm>
#include <cmath>

// =====================================================
//  Shader sources
// =====================================================

// Vertex shader: reconstructs a camera-facing quad from gl_VertexID.
// Reads one GPUParticle from the SSBO using gl_InstanceID.
static const char* kParticleVert = R"GLSL(
    #version 430 core

    struct GPUParticle {
        vec4 positionSize; // xyz = world pos, w = size
        vec4 color;
    };
    layout(std430, binding = 1) readonly buffer ParticleBuffer {
        GPUParticle particles[];
    };

    uniform mat4 uView;
    uniform mat4 uProjection;

    out vec2 vUV;
    out vec4 vColor;

    const vec2 kCorners[6] = vec2[6](
        vec2(-0.5,  0.5),
        vec2(-0.5, -0.5),
        vec2( 0.5, -0.5),
        vec2(-0.5,  0.5),
        vec2( 0.5, -0.5),
        vec2( 0.5,  0.5)
    );
    const vec2 kUVs[6] = vec2[6](
        vec2(0.0, 1.0),
        vec2(0.0, 0.0),
        vec2(1.0, 0.0),
        vec2(0.0, 1.0),
        vec2(1.0, 0.0),
        vec2(1.0, 1.0)
    );

    void main()
    {
        GPUParticle p  = particles[gl_InstanceID];
        vec3  worldPos = p.positionSize.xyz;
        float size     = p.positionSize.w;

        // Camera right/up from view matrix rows (billboard axes)
        vec3 right = vec3(uView[0][0], uView[1][0], uView[2][0]);
        vec3 up    = vec3(uView[0][1], uView[1][1], uView[2][1]);

        vec2 corner  = kCorners[gl_VertexID] * size;
        vec3 vertPos = worldPos + right * corner.x + up * corner.y;

        vUV         = kUVs[gl_VertexID];
        vColor      = p.color;
        gl_Position = uProjection * uView * vec4(vertPos, 1.0);
    }
)GLSL";

// Fragment shader: radial soft-edge circle.
static const char* kParticleFrag = R"GLSL(
    #version 430 core
    in  vec2 vUV;
    in  vec4 vColor;
    out vec4 FragColor;

    void main()
    {
        vec2  uv    = vUV * 2.0 - 1.0;
        float dist  = length(uv);
        float alpha = smoothstep(1.0, 0.5, dist);
        FragColor   = vec4(vColor.rgb, vColor.a * alpha);
    }
)GLSL";

// =====================================================
//  Init
// =====================================================
void ParticleSystem::Init()
{
    CompileShader();
    InitQuadVAO();

    glGenBuffers(1, &m_ssbo);
    EnsureSSBOCapacity(512);

    // FIX #6 — cache uniform locations once, not every Draw() call
    m_uView = glGetUniformLocation(m_shader, "uView");
    m_uProjection = glGetUniformLocation(m_shader, "uProjection");
}

// =====================================================
//  Update — simulate all emitters, fill staging buffers
// =====================================================
void ParticleSystem::Update(EntityManager& entityManager,
    std::vector<EventEntry>& /*events*/,
    bool isServer,
    float deltaTime)
{
    if (isServer) return;

    // FIX #8 — clear staging at the top of Update, not the bottom of Draw.
    // If Draw() is never called (e.g. emitter is culled), the buffer won't
    // grow without bound.
    m_stagingAdditive.clear();
    m_stagingAlpha.clear();

    auto query = entityManager.CreateQuery<ParticleEmitterComponent, Transform>();

    for (auto [entity, emitter, transform] : query)
    {
        glm::vec3 worldPos = transform->getPosition();
        glm::mat4 model = transform->getModelMatrix();

        float scaleX = glm::length(glm::vec3(model[0]));
        float scaleY = glm::length(glm::vec3(model[1]));
        float scaleZ = glm::length(glm::vec3(model[2]));
        float uniformScale = (scaleX + scaleY + scaleZ) / 3.0f;

        glm::vec3 worldDir = glm::normalize(glm::vec3(-model[2]));

        SimulateEmitter(*emitter, worldPos, worldDir, uniformScale, deltaTime);

        if (emitter->done) 
        {
			entityManager.DestroyEntity(entity);
        }
    }
}

// =====================================================
//  Draw — upload and issue draw calls per blend mode
// =====================================================
void ParticleSystem::Draw(const glm::mat4& view, const glm::mat4& projection)
{
    if (m_shader == 0) return;
    if (m_stagingAdditive.empty() && m_stagingAlpha.empty()) return;

    glUseProgram(m_shader);
    // FIX #6 — use cached locations
    glUniformMatrix4fv(m_uView, 1, GL_FALSE, glm::value_ptr(view));
    glUniformMatrix4fv(m_uProjection, 1, GL_FALSE, glm::value_ptr(projection));

    glDepthMask(GL_FALSE);
    glEnable(GL_BLEND);
    glBindVertexArray(m_quadVAO);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, m_ssbo);

    // FIX #7 — separate draw calls for each blend mode so smoke and fire
    // can coexist correctly.  Alpha-blended particles are drawn first so
    // additive particles composite on top of them.
    FlushStagingBuffer(m_stagingAlpha,    /*additive=*/false);
    FlushStagingBuffer(m_stagingAdditive, /*additive=*/true);

    glBindVertexArray(0);
    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);
    glUseProgram(0);
}

// =====================================================
//  FlushStagingBuffer — upload one staging batch and draw
// =====================================================
void ParticleSystem::FlushStagingBuffer(std::vector<GPUParticle>& src,
    bool additive)
{
    if (src.empty()) return;

    // FIX #7 — set blend mode per batch
    if (additive)
        glBlendFunc(GL_SRC_ALPHA, GL_ONE);
    else
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    EnsureSSBOCapacity(static_cast<int>(src.size()));

    glBindBuffer(GL_SHADER_STORAGE_BUFFER, m_ssbo);
    glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0,
        src.size() * sizeof(GPUParticle),
        src.data());

    glDrawArraysInstanced(GL_TRIANGLES, 0, 6,
        static_cast<GLsizei>(src.size()));
}

// =====================================================
//  SimulateEmitter
// =====================================================
void ParticleSystem::SimulateEmitter(ParticleEmitterComponent& e,
    const glm::vec3& emitterWorldPos,
    const glm::vec3& emitterWorldDir,
    float uniformScale,
    float dt)
{
    static const glm::vec3 kGravity(0.0f, -9.81f, 0.0f);

    EnsurePool(e);

    e.elapsedTime += dt;

    // ---- Spawn new particles ----
    if (e.enabled && (e.looping || e.elapsedTime <= e.duration))
    {
        float flickeredRate = e.emissionRate * e.speedScale;
        if (e.emissionVariance > 0.0f)
            flickeredRate += (RandF() * 2.0f - 1.0f) * e.emissionVariance;
        flickeredRate = std::max(0.0f, flickeredRate);

        e.emissionAccum += flickeredRate * dt;
        int toSpawn = static_cast<int>(e.emissionAccum);
        e.emissionAccum -= static_cast<float>(toSpawn);

        for (int i = 0; i < toSpawn; ++i)
            SpawnParticle(e, emitterWorldPos, emitterWorldDir, uniformScale);
    }

    // ---- Integrate alive particles ----
    e.aliveCount = 0;

    // Choose the right staging buffer for this emitter's blend mode
    auto& staging = e.additiveBlend ? m_stagingAdditive : m_stagingAlpha;

    for (int idx = 0; idx < static_cast<int>(e.pool.size()); ++idx)
    {
        Particle& p = e.pool[idx];
        if (!p.alive) continue;

        p.age += dt;
        if (p.age >= p.lifetime)
        {
            p.alive = false;
            // FIX #2 — return slot to free-list in O(1)
            e.freeList.push_back(idx);
            continue;
        }

        // Physics
        p.velocity += kGravity * e.gravityModifier * dt;

        if (e.turbulenceStrength > 0.0f)
        {
            glm::vec3 turb(
                (RandF() * 2.0f - 1.0f) * e.turbulenceStrength,
                (RandF() * 2.0f - 1.0f) * e.turbulenceStrength,
                (RandF() * 2.0f - 1.0f) * e.turbulenceStrength
            );
            p.velocity += turb * dt;
        }

        // FIX: SimulationSpace::Local — offset particle by emitter movement
        if (e.simulationSpace == SimulationSpace::Local)
        {
            // The caller passes the current world position every frame.
            // We store the last known emitter position in the component and
            // move all local-space particles by the delta each tick.
            // (emitterLastPos is updated at the end of this function.)
            p.position += emitterWorldPos - e.emitterLastPos;
        }

        p.position += p.velocity * dt;

        if (e.onUpdate) e.onUpdate(p, dt);

        ++e.aliveCount;

        float     t = p.age / p.lifetime;
        glm::vec4 color = glm::mix(p.colorStart, p.colorEnd, t);
        float     size = glm::mix(p.sizeStart, p.sizeEnd, t);

        staging.push_back({ glm::vec4(p.position, size), color });
    }

    // Record emitter position so Local-space particles can track it next frame
    e.emitterLastPos = emitterWorldPos;

    // Mark done once a non-looping emitter has passed its duration and every
    // particle has died.  Game logic can poll e.done to remove/recycle the entity.
    if (!e.looping && !e.done
        && e.elapsedTime > e.duration
        && e.aliveCount == 0)
    {
        e.done = true;
    }
}

// =====================================================
//  SpawnParticle
// =====================================================
void ParticleSystem::SpawnParticle(ParticleEmitterComponent& e,
    const glm::vec3& emitterWorldPos,
    const glm::vec3& emitterWorldDir,
    float uniformScale)
{
    // FIX #2 — O(1) slot lookup via free-list
    if (e.freeList.empty()) return;  // pool exhausted

    int   idx = e.freeList.back();
    e.freeList.pop_back();
    Particle& slot = e.pool[idx];

    // Lifetime variance
    float lifetime = e.startLifetime * e.speedScale;
    if (e.lifetimeVariance > 0.0f)
        lifetime += (RandF() * 2.0f - 1.0f) * e.lifetimeVariance;
    lifetime = std::max(0.05f, lifetime);

    // Speed variance
    float speed = e.startSpeed;
    if (e.speedVariance > 0.0f)
        speed += (RandF() * 2.0f - 1.0f) * e.speedVariance;
    speed = std::max(0.0f, speed);

    // FIX #3 — SampleSpawnVelocity now returns coneT (normalised angular
    // deviation) so colorTemperature uses that directly instead of the
    // dot-product of a direction already derived from emitterWorldDir.
    float     coneT = 0.0f;
    glm::vec3 spawnDir = SampleSpawnVelocity(e, emitterWorldDir, coneT);

    // FIX #4 (colorTemperature) — heat is driven by coneT (0 = axis, 1 = edge)
    // so hotter particles are those closest to the cone centre, not those
    // whose direction accidentally dot-products high against the same axis.
    glm::vec4 spawnColor = e.startColor;
    if (e.colorTemperature && e.shape == EmitterShape::Cone)
    {
        float heatT = 1.0f - coneT;  // axis-aligned → hottest
        glm::vec4 hotColor = glm::vec4(1.0f, 1.0f, 1.0f, e.startColor.a);
        spawnColor = glm::mix(e.startColor, hotColor, heatT);
    }

    slot.alive = true;
    slot.age = 0.0f;
    slot.lifetime = lifetime;
    slot.colorStart = spawnColor;
    slot.colorEnd = e.endColor;
    slot.sizeStart = e.startSize * uniformScale;
    slot.sizeEnd = e.endSize * uniformScale;
    // FIX: removed the misleading emitterWorldPos argument from SampleSpawnPosition
    slot.position = emitterWorldPos + SampleSpawnPosition(e) * uniformScale;
    slot.velocity = spawnDir * speed;
}

// =====================================================
//  SampleSpawnPosition
//  Returns a local-space offset (before uniformScale).
//  emitterWorldPos is not needed here — removed from signature.
// =====================================================
glm::vec3 ParticleSystem::SampleSpawnPosition(const ParticleEmitterComponent& e)
{
    switch (e.shape)
    {
    case EmitterShape::Sphere:
    {
        // Uniform point inside a sphere — rejection sample
        glm::vec3 p;
        do {
            p = glm::vec3(RandF() * 2.0f - 1.0f,
                RandF() * 2.0f - 1.0f,
                RandF() * 2.0f - 1.0f);
        } while (glm::dot(p, p) > 1.0f);
        return p * e.shapeRadius;
    }
    case EmitterShape::Cone:
    case EmitterShape::Point:
    default:
        return glm::vec3(0.0f);
    }
}

// =====================================================
//  SampleSpawnVelocity
//  outConeT: normalised [0,1] deviation from cone axis.
//            0 = dead centre, 1 = cone edge.
//            Only meaningful for EmitterShape::Cone.
// =====================================================
glm::vec3 ParticleSystem::SampleSpawnVelocity(const ParticleEmitterComponent& e,
    const glm::vec3& emitterWorldDir,
    float& outConeT)
{
    outConeT = 0.0f;

    switch (e.shape)
    {
    case EmitterShape::Cone:
    {
        // FIX #3 — uniform solid-angle sample within the cone.
        // Linear angle sampling (old code) biases toward the axis because
        // equal angle steps cover less solid angle near the centre.
        // Correct method: sample cos(angle) uniformly in [cos(halfAngle), 1].
        float cosMax = std::cos(e.shapeConeAngle);
        float cosA = cosMax + RandF() * (1.0f - cosMax);  // uniform in solid angle
        float sinA = std::sqrt(std::max(0.0f, 1.0f - cosA * cosA));
        float phi = RandF() * 6.2831853f;

        // outConeT: 0 when cosA==1 (axis), 1 when cosA==cosMax (edge)
        outConeT = (cosA < 1.0f)
            ? (1.0f - cosA) / (1.0f - cosMax)
            : 0.0f;

        glm::vec3 perp = (glm::abs(emitterWorldDir.x) < 0.9f)
            ? glm::vec3(1, 0, 0)
            : glm::vec3(0, 1, 0);
        glm::vec3 right = glm::normalize(glm::cross(emitterWorldDir, perp));
        glm::vec3 up = glm::cross(emitterWorldDir, right);

        return glm::normalize(emitterWorldDir * cosA
            + right * (sinA * std::cos(phi))
            + up * (sinA * std::sin(phi)));
    }
    case EmitterShape::Sphere:
    {
        // Uniform direction on unit sphere — rejection sample
        glm::vec3 d;
        do {
            d = glm::vec3(RandF() * 2.0f - 1.0f,
                RandF() * 2.0f - 1.0f,
                RandF() * 2.0f - 1.0f);
        } while (glm::dot(d, d) < 0.0001f);
        return glm::normalize(d);
    }
    case EmitterShape::Point:
    default:
        return emitterWorldDir;
    }
}

// =====================================================
//  EnsurePool
//  Initialises or resizes the particle pool and free-list.
// =====================================================
void ParticleSystem::EnsurePool(ParticleEmitterComponent& e)
{
    const int target = e.maxParticles;
    const int current = static_cast<int>(e.pool.size());
    if (current == target) return;

    if (current < target)
    {
        // Growing: append new dead particles and add their indices to the list
        e.pool.resize(target);
        for (int i = current; i < target; ++i)
        {
            e.pool[i] = Particle{};           // ensure alive == false
            e.freeList.push_back(i);
        }
    }
    else
    {
        // Shrinking: kill any particles beyond the new limit and rebuild list
        e.pool.resize(target);
        e.freeList.clear();
        for (int i = 0; i < target; ++i)
            if (!e.pool[i].alive)
                e.freeList.push_back(i);
    }
}

// =====================================================
//  GPU helpers
// =====================================================
void ParticleSystem::EnsureSSBOCapacity(int needed)
{
    if (needed <= m_ssboCapacity) return;

    // FIX #1 — proper power-of-two round-up.
    // Old code set newCap = max(needed, 512) then looped while (newCap < needed),
    // which never executed since newCap was already >= needed.
    int newCap = 512;
    while (newCap < needed) newCap *= 2;

    glBindBuffer(GL_SHADER_STORAGE_BUFFER, m_ssbo);
    glBufferData(GL_SHADER_STORAGE_BUFFER,
        newCap * sizeof(GPUParticle), nullptr, GL_DYNAMIC_DRAW);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);

    m_ssboCapacity = newCap;
}

void ParticleSystem::CompileShader()
{
    auto compileStage = [](GLenum type, const char* src) -> GLuint {
        GLuint s = glCreateShader(type);
        glShaderSource(s, 1, &src, nullptr);
        glCompileShader(s);
        GLint ok = 0;
        glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
        if (!ok) {
            char log[512];
            glGetShaderInfoLog(s, 512, nullptr, log);
            Debug::Error("ParticleSystem::Shader") << log << "\n";
        }
        return s;
        };

    GLuint vert = compileStage(GL_VERTEX_SHADER, kParticleVert);
    GLuint frag = compileStage(GL_FRAGMENT_SHADER, kParticleFrag);

    m_shader = glCreateProgram();
    glAttachShader(m_shader, vert);
    glAttachShader(m_shader, frag);
    glLinkProgram(m_shader);

    GLint ok = 0;
    glGetProgramiv(m_shader, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetProgramInfoLog(m_shader, 512, nullptr, log);
        Debug::Error("ParticleSystem::Program") << log << "\n";
    }

    glDetachShader(m_shader, vert); glDeleteShader(vert);
    glDetachShader(m_shader, frag); glDeleteShader(frag);
}

void ParticleSystem::InitQuadVAO()
{
    // No vertex data — positions built entirely in the vertex shader.
    glGenVertexArrays(1, &m_quadVAO);
}

// =====================================================
//  Destructor
// =====================================================
ParticleSystem::~ParticleSystem()
{
    glDeleteProgram(m_shader);
    glDeleteVertexArrays(1, &m_quadVAO);
    glDeleteBuffers(1, &m_ssbo);
}