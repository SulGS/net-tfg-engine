#include "ParticleSystem.hpp"
#include "Utils/Debug/Debug.hpp"
#include <glm/gtc/type_ptr.hpp>
#include <algorithm>
#include <cmath>
#include <cstdlib>

// =====================================================
//  Shader sources
// =====================================================

// Vertex shader: reconstructs a camera-facing quad from gl_VertexID.
// Reads one GPUParticle from the SSBO using gl_InstanceID.
static const char* kParticleVert = R"GLSL(
    #version 430 core

    // Per-instance data from SSBO (std430)
    struct GPUParticle {
        vec4 positionSize; // xyz = world pos, w = size
        vec4 color;
    };
    layout(std430, binding = 1) readonly buffer ParticleBuffer {
        GPUParticle particles[];
    };

    uniform mat4 uView;
    uniform mat4 uProjection;

    out vec2  vUV;
    out vec4  vColor;

    // Unit quad corners in billboard (camera) space
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
        GPUParticle p = particles[gl_InstanceID];

        vec3  worldPos = p.positionSize.xyz;
        float size     = p.positionSize.w;

        // Camera right/up from the view matrix rows (billboard axes)
        vec3 right = vec3(uView[0][0], uView[1][0], uView[2][0]);
        vec3 up    = vec3(uView[0][1], uView[1][1], uView[2][1]);

        vec2 corner   = kCorners[gl_VertexID] * size;
        vec3 vertPos  = worldPos + right * corner.x + up * corner.y;

        vUV    = kUVs[gl_VertexID];
        vColor = p.color;

        gl_Position = uProjection * uView * vec4(vertPos, 1.0);
    }
)GLSL";

// Fragment shader: radial soft-edge circle, alpha from colour.
static const char* kParticleFrag = R"GLSL(
    #version 430 core
    in  vec2 vUV;
    in  vec4 vColor;
    out vec4 FragColor;

    void main()
    {
        // Signed distance from centre → soft circular billboard
        vec2  uv   = vUV * 2.0 - 1.0;
        float dist = length(uv);
        float alpha = smoothstep(1.0, 0.5, dist);

        FragColor = vec4(vColor.rgb, vColor.a * alpha);
    }
)GLSL";

// =====================================================
//  Init
// =====================================================
void ParticleSystem::Init()
{
    CompileShader();
    InitQuadVAO();

    // Create SSBO with a reasonable initial capacity
    glGenBuffers(1, &m_ssbo);
    EnsureSSBOCapacity(512);
}

// =====================================================
//  Update — simulate all emitters, upload to GPU
// =====================================================
void ParticleSystem::Update(EntityManager& entityManager,
    std::vector<EventEntry>& /*events*/,
    bool isServer,
    float deltaTime)
{
    if (isServer) return;

    auto query = entityManager.CreateQuery<ParticleEmitterComponent, Transform>();

    for (auto [entity, emitter, transform] : query)
    {

        glm::vec3 worldPos = transform->getPosition();

        // Extract axes from model matrix
        glm::mat4 model = transform->getModelMatrix();

        // Scale: length of each basis column
        float scaleX = glm::length(glm::vec3(model[0]));
        float scaleY = glm::length(glm::vec3(model[1]));
        float scaleZ = glm::length(glm::vec3(model[2]));
        float uniformScale = (scaleX + scaleY + scaleZ) / 3.0f;

        // Forward direction: -Z column, normalised (remove scale)
        glm::vec3 worldDir = glm::normalize(glm::vec3(-model[2]));

        SimulateEmitter(*emitter, worldPos, worldDir, uniformScale, deltaTime);
    }
}

// =====================================================
//  Draw — upload staging data, one draw call per emitter
// =====================================================
void ParticleSystem::Draw(const glm::mat4& view, const glm::mat4& projection)
{
    if (m_shader == 0) return;

    glUseProgram(m_shader);
    glUniformMatrix4fv(glGetUniformLocation(m_shader, "uView"), 1, GL_FALSE, glm::value_ptr(view));
    glUniformMatrix4fv(glGetUniformLocation(m_shader, "uProjection"), 1, GL_FALSE, glm::value_ptr(projection));

    // Additive blending — works well for fire/sparks/magic
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE);
    glDepthMask(GL_FALSE);   // don't write depth; particles are transparent

    glBindVertexArray(m_quadVAO);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, m_ssbo);

    // We need access to every emitter again — store results from Update.
    // Simple approach: re-iterate (entityManager not available here, so
    // callers must supply particles via DrawEmitter called from Update,
    // OR we cache the staging data per emitter below).
    //
    // Here we use a single merged draw strategy:
    //   • All alive particles from all emitters are packed into m_staging.
    //   • One draw call for all of them (no per-emitter state difference).
    //   This works because all emitters share the same billboard shader.

    if (!m_staging.empty())
    {
        EnsureSSBOCapacity(static_cast<int>(m_staging.size()));

        glBindBuffer(GL_SHADER_STORAGE_BUFFER, m_ssbo);
        glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0,
            m_staging.size() * sizeof(GPUParticle),
            m_staging.data());

        glDrawArraysInstanced(GL_TRIANGLES, 0,
            6,  // 2 triangles = 6 verts per billboard
            static_cast<GLsizei>(m_staging.size()));

        m_staging.clear();
    }

    glBindVertexArray(0);
    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);
    glUseProgram(0);
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
    auto randF = []() { return static_cast<float>(std::rand()) / static_cast<float>(RAND_MAX); };

    e.elapsedTime += dt;

    // ---- Spawn new particles ----
    if (e.enabled && (e.looping || e.elapsedTime <= e.duration))
    {
        // Flicker: jitter emission rate +/- emissionVariance each frame
        float flickeredRate = e.emissionRate * e.speedScale;
        if (e.emissionVariance > 0.0f)
            flickeredRate += (randF() * 2.0f - 1.0f) * e.emissionVariance;
        flickeredRate = std::max(0.0f, flickeredRate);

        e.emissionAccum += flickeredRate * dt;
        int toSpawn = static_cast<int>(e.emissionAccum);
        e.emissionAccum -= static_cast<float>(toSpawn);

        for (int i = 0; i < toSpawn; ++i)
            SpawnParticle(e, emitterWorldPos, emitterWorldDir, uniformScale);
    }

    // ---- Integrate alive particles ----
    e.aliveCount = 0;
    for (auto& p : e.pool)
    {
        if (!p.alive) continue;

        p.age += dt;
        if (p.age >= p.lifetime) { p.alive = false; continue; }

        // Physics integration
        p.velocity += kGravity * e.gravityModifier * dt;

        // Turbulence: small random impulse each frame
        if (e.turbulenceStrength > 0.0f)
        {
            glm::vec3 turbulence(
                (randF() * 2.0f - 1.0f) * e.turbulenceStrength,
                (randF() * 2.0f - 1.0f) * e.turbulenceStrength,
                (randF() * 2.0f - 1.0f) * e.turbulenceStrength
            );
            p.velocity += turbulence * dt;
        }

        p.position += p.velocity * dt;

        // Optional custom hook
        if (e.onUpdate) e.onUpdate(p, dt);

        ++e.aliveCount;

        // Pack into staging buffer for Draw()
        float t = p.age / p.lifetime;
        glm::vec4 color = glm::mix(p.colorStart, p.colorEnd, t);
        float size = glm::mix(p.sizeStart, p.sizeEnd, t);

        m_staging.push_back({ glm::vec4(p.position, size), color });
    }
}

// =====================================================
//  SpawnParticle — find a dead slot and initialise it
// =====================================================
void ParticleSystem::SpawnParticle(ParticleEmitterComponent& e,
    const glm::vec3& emitterWorldPos,
    const glm::vec3& emitterWorldDir,
    float uniformScale)
{
    // Ensure pool is sized
    if (static_cast<int>(e.pool.size()) < e.maxParticles)
        e.pool.resize(e.maxParticles);

    // Find first dead particle
    Particle* slot = nullptr;
    for (auto& p : e.pool)
    {
        if (!p.alive) { slot = &p; break; }
    }
    if (!slot) return; // pool exhausted

    auto randF = []() { return static_cast<float>(std::rand()) / static_cast<float>(RAND_MAX); };

    // Lifetime variance
    float lifetime = e.startLifetime * e.speedScale;
    if (e.lifetimeVariance > 0.0f)
        lifetime += (randF() * 2.0f - 1.0f) * e.lifetimeVariance;
    lifetime = std::max(0.05f, lifetime);

    // Speed variance
    float speed = e.startSpeed;
    if (e.speedVariance > 0.0f)
        speed += (randF() * 2.0f - 1.0f) * e.speedVariance;
    speed = std::max(0.0f, speed);

    // Color temperature: only when opted in — never override smoke/fire colors
    glm::vec3 spawnDir = SampleSpawnVelocity(e, emitterWorldDir);
    glm::vec4 spawnColor = e.startColor;
    if (e.colorTemperature)
    {
        float alignment = glm::dot(spawnDir, emitterWorldDir);
        float heatT = glm::clamp((alignment - 0.95f) / 0.05f, 0.0f, 1.0f);
        glm::vec4 hotColor = glm::vec4(1.0f, 1.0f, 1.0f, e.startColor.a);
        spawnColor = glm::mix(e.startColor, hotColor, heatT);
    }

    slot->alive = true;
    slot->age = 0.0f;
    slot->lifetime = lifetime;
    slot->colorStart = spawnColor;
    slot->colorEnd = e.endColor;
    slot->sizeStart = e.startSize * uniformScale;
    slot->sizeEnd = e.endSize * uniformScale;
    slot->position = emitterWorldPos + SampleSpawnPosition(e, emitterWorldPos) * uniformScale;
    slot->velocity = spawnDir * speed;
}

// =====================================================
//  SampleSpawnPosition
// =====================================================
glm::vec3 ParticleSystem::SampleSpawnPosition(const ParticleEmitterComponent& e,
    const glm::vec3& /*emitterWorldPos*/)
{
    auto randF = []() { return static_cast<float>(std::rand()) / static_cast<float>(RAND_MAX); };

    switch (e.shape)
    {
    case EmitterShape::Sphere:
    {
        // Uniform point in sphere (rejection sample)
        glm::vec3 p;
        do {
            p = glm::vec3(randF() * 2.0f - 1.0f, randF() * 2.0f - 1.0f, randF() * 2.0f - 1.0f);
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
// =====================================================
glm::vec3 ParticleSystem::SampleSpawnVelocity(const ParticleEmitterComponent& e,
    const glm::vec3& emitterWorldDir)
{
    auto randF = []() { return static_cast<float>(std::rand()) / static_cast<float>(RAND_MAX); };

    switch (e.shape)
    {
    case EmitterShape::Cone:
    {
        // Random direction within cone angle around emitterWorldDir
        float angle = randF() * e.shapeConeAngle;
        float phi = randF() * 6.2831853f;
        // Perturb direction by constructing an arbitrary perpendicular
        glm::vec3 perp = glm::abs(emitterWorldDir.x) < 0.9f
            ? glm::vec3(1, 0, 0) : glm::vec3(0, 1, 0);
        glm::vec3 right = glm::normalize(glm::cross(emitterWorldDir, perp));
        glm::vec3 up = glm::cross(emitterWorldDir, right);
        float s = std::sin(angle);
        return glm::normalize(emitterWorldDir
            + right * (s * std::cos(phi))
            + up * (s * std::sin(phi)));
    }
    case EmitterShape::Sphere:
    {
        // Random direction on unit sphere
        glm::vec3 d;
        do {
            d = glm::vec3(randF() * 2.0f - 1.0f, randF() * 2.0f - 1.0f, randF() * 2.0f - 1.0f);
        } while (glm::dot(d, d) < 0.0001f);
        return glm::normalize(d);
    }
    case EmitterShape::Point:
    default:
        return emitterWorldDir;
    }
}

// =====================================================
//  GPU helpers
// =====================================================
void ParticleSystem::EnsureSSBOCapacity(int needed)
{
    if (needed <= m_ssboCapacity) return;

    // Round up to next power of two for fewer reallocations
    int newCap = std::max(needed, 512);
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
    // No vertex data needed — positions are built entirely in the vertex
    // shader from gl_VertexID.  We still need a bound VAO.
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