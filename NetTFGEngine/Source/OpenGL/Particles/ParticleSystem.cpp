#include "ParticleSystem.hpp"
#include "Utils/AssetManager.hpp"
#include "Utils/Debug/Debug.hpp"
#include <glm/gtc/type_ptr.hpp>
#include <algorithm>
#include <cmath>

// Vertex shader: reconstructs a camera-facing quad from gl_VertexID.
// Reads one GPUParticle from the SSBO using gl_InstanceID. Shared by the colour and distortion programs.
static const char* kParticleVert = R"GLSL(
    #version 430 core

    struct GPUParticle {
        vec4 positionSize; // xyz = world pos, w = size
        vec4 color;
        vec4 velocitySeed; // xyz = world velocity, w = seed
        vec4 params;       // x = stretch, y = noiseAmount / distortion strength, z = rotation, w = normalised age
        vec4 flip;         // x = cols, y = rows, z = frame, w = frame count
    };
    layout(std430, binding = 1) readonly buffer ParticleBuffer {
        GPUParticle particles[];
    };

    uniform mat4 uView;
    uniform mat4 uProjection;

    out vec2 vUV;
    out vec4 vColor;
    out vec4 vParams; // x = seed, y = noiseAmount / distortion strength, z = rotation, w = normalised age
    out vec4 vFlip;

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

        vec2 local = kCorners[gl_VertexID];
        vec2 corner;

        // Streak: elongate the quad along the on-screen velocity (uses the screen-plane
        // component only, so a particle flying at the camera does not turn into a long bar).
        vec2  vScreen = vec2(dot(p.velocitySeed.xyz, right), dot(p.velocitySeed.xyz, up));
        float speed   = length(vScreen);
        if (p.params.x > 0.0 && speed > 0.001)
        {
            vec2 axis = vScreen / speed;
            vec2 perp = vec2(-axis.y, axis.x);
            corner = axis * (local.x * (size + p.params.x * speed)) + perp * (local.y * size);
        }
        else
        {
            // Spin the sprite in the view plane (rotation is 0 for distortion rings).
            float cr = cos(p.params.z);
            float sr = sin(p.params.z);
            local  = vec2(cr * local.x - sr * local.y, sr * local.x + cr * local.y);
            corner = local * size;
        }
        vec3 vertPos = worldPos + right * corner.x + up * corner.y;

        vUV         = kUVs[gl_VertexID];
        vColor      = p.color;
        vParams     = vec4(p.velocitySeed.w, p.params.y, p.params.z, p.params.w);
        vFlip       = p.flip;
        gl_Position = uProjection * uView * vec4(vertPos, 1.0);
    }
)GLSL";

// Fragment shader: three looks.
//  - procedural disc: radial soft-edge circle, optionally perturbed by fractal value noise (rotated by the quad, offset per particle) that erodes as the particle ages, giving irregular wispy fire/smoke puffs
//  - flipbook: samples a sprite sheet, cross-fading between two adjacent frames
static const char* kParticleFrag = R"GLSL(
    #version 430 core
    in  vec2 vUV;
    in  vec4 vColor;
    in  vec4 vParams; // x = seed, y = noiseAmount, z = rotation, w = normalised age
    in  vec4 vFlip;   // x = cols, y = rows, z = frame, w = frame count
    out vec4 FragColor;

    uniform sampler2D uTex;
    uniform int       uAlphaMode; // 0 = procedural, 1 = texture alpha, 2 = luminance mask, 3 = additive

    float hash(vec2 p)
    {
        p = fract(p * vec2(123.34, 456.21));
        p += dot(p, p + 45.32);
        return fract(p.x * p.y);
    }

    float vnoise(vec2 p)
    {
        vec2 i = floor(p);
        vec2 f = fract(p);
        f = f * f * (3.0 - 2.0 * f);
        return mix(mix(hash(i),                hash(i + vec2(1.0, 0.0)), f.x),
                   mix(hash(i + vec2(0.0, 1.0)), hash(i + vec2(1.0, 1.0)), f.x), f.y);
    }

    float fbm(vec2 p)
    {
        float v = 0.0;
        float a = 0.5;
        for (int i = 0; i < 3; ++i)
        {
            v += a * vnoise(p);
            p  = p * 2.03 + vec2(17.1, 9.7);
            a *= 0.5;
        }
        return v; // 0 .. 0.875, mean ~0.44
    }

    // Frame 0 = top-left; the sheet is uploaded with Y inverted, so image top is v = 1.
    vec4 SampleFrame(float frame, vec2 uv01)
    {
        float cols = max(vFlip.x, 1.0);
        float rows = max(vFlip.y, 1.0);
        float col  = mod(frame, cols);
        float row  = floor(frame / cols);
        uv01 = clamp(uv01, vec2(0.003), vec2(0.997)); // keep bilinear taps inside the cell
        vec2 uv = vec2((col + uv01.x) / cols, 1.0 - (row + 1.0 - uv01.y) / rows);
        return texture(uTex, uv);
    }

    void main()
    {
        if (uAlphaMode > 0)
        {
            float f0 = floor(vFlip.z);
            float f1 = min(f0 + 1.0, max(vFlip.w - 1.0, 0.0));
            float b  = vFlip.z - f0;

            vec4 t = SampleFrame(f0, vUV);
            if (b > 0.001 && f1 > f0)
                t = mix(t, SampleFrame(f1, vUV), b);

            // Black level: texture compression leaves faint non-zero noise where the sheet is black, and HDR tints multiply it into visible square outlines around additive sprites.
            t.rgb = max(t.rgb - vec3(0.03), vec3(0.0)) * (1.0 / 0.97);

            vec3  rgb;
            float a;
            if (uAlphaMode == 1)      { rgb = t.rgb * vColor.rgb; a = t.a * vColor.a; }
            else if (uAlphaMode == 2) { rgb = vColor.rgb; a = dot(t.rgb, vec3(0.299, 0.587, 0.114)) * vColor.a; }
            else                      { rgb = t.rgb * vColor.rgb; a = vColor.a; }

            FragColor = vec4(rgb, a);
            return;
        }

        vec2  uv   = vUV * 2.0 - 1.0;
        float dist = length(uv);
        float shape;

        if (vParams.y > 0.0)
        {
            float n     = fbm(uv * 2.2 + vParams.x * 37.0);
            float erode = 0.85 + 0.7 * vParams.w;
            float d     = dist + (n - 0.44) * vParams.y * erode * 1.6;

            // Force a fade at the quad border so the noise never shows a hard cut.
            shape = smoothstep(1.0, 0.35, d) * smoothstep(1.0, 0.8, dist);
        }
        else
        {
            shape = smoothstep(1.0, 0.5, dist);
        }

        FragColor = vec4(vColor.rgb, vColor.a * shape);
    }
)GLSL";

// Distortion fragment shader: instead of colour, refract the scene copy through a thin radial ring. The ring sits near the sprite edge; vColor.a is the strength envelope over the particle's life, vParams.y the peak displacement (fraction of screen height). Red/green/blue are displaced by slightly different amounts for a subtle chromatic fringe.
static const char* kDistortionFrag = R"GLSL(
    #version 430 core
    in  vec2 vUV;
    in  vec4 vColor;
    in  vec4 vParams; // y = strength
    in  vec4 vFlip;
    out vec4 FragColor;

    uniform sampler2D uScene;
    uniform vec2      uViewport;

    void main()
    {
        vec2  uv = vUV * 2.0 - 1.0;
        float d  = length(uv);
        if (d >= 1.0) discard;

        float ring = smoothstep(0.45, 0.8, d) * (1.0 - smoothstep(0.8, 1.0, d));
        float amt  = ring * vParams.y * vColor.a;
        if (amt <= 0.0) discard;

        vec2 dir = uv / max(d, 1e-4);
        // Sample from nearer the centre so the content looks pushed outward by the front.
        vec2 off = -dir * amt * (uViewport.y / uViewport);
        vec2 suv = gl_FragCoord.xy / uViewport;

        vec3 c;
        c.r = texture(uScene, suv + off * 1.10).r;
        c.g = texture(uScene, suv + off       ).g;
        c.b = texture(uScene, suv + off * 0.90).b;

        FragColor = vec4(c, clamp(ring * vColor.a * 4.0, 0.0, 1.0));
    }
)GLSL";

// Piecewise-linear lookup in the emitter's colour ramp (stops assumed sorted by t).
static glm::vec4 EvalColorRamp(const ParticleEmitterComponent& e, float t)
{
    const int n = std::min(e.colorRampCount, ParticleEmitterComponent::kMaxColorStops);
    if (t <= e.colorRamp[0].t) return e.colorRamp[0].color;
    for (int i = 1; i < n; ++i)
    {
        if (t <= e.colorRamp[i].t)
        {
            const ColorStop& a = e.colorRamp[i - 1];
            const ColorStop& b = e.colorRamp[i];
            float k = (t - a.t) / std::max(b.t - a.t, 1e-5f);
            return glm::mix(a.color, b.color, k);
        }
    }
    return e.colorRamp[n - 1].color;
}

void ParticleSystem::Init()
{
    CompileShaders();
    InitQuadVAO();

    glGenBuffers(1, &m_ssbo);
    EnsureSSBOCapacity(512);

    // FIX #6 — cache uniform locations once, not every Draw() call
    m_uView = glGetUniformLocation(m_shader, "uView");
    m_uProjection = glGetUniformLocation(m_shader, "uProjection");
    m_uTex = glGetUniformLocation(m_shader, "uTex");
    m_uAlphaMode = glGetUniformLocation(m_shader, "uAlphaMode");

    m_dView = glGetUniformLocation(m_distShader, "uView");
    m_dProjection = glGetUniformLocation(m_distShader, "uProjection");
    m_dScene = glGetUniformLocation(m_distShader, "uScene");
    m_dViewport = glGetUniformLocation(m_distShader, "uViewport");
}

// Update — simulate all emitters, fill staging buffers
void ParticleSystem::Update(EntityManager& entityManager,
    std::vector<EventEntry>& /*events*/,
    bool isServer,
    float deltaTime)
{
    if (isServer) return;

    // FIX #8 — clear staging at the top of Update, not the bottom of Draw.
    // If Draw() is never called (e.g. emitter is culled), the buffer won't
    // grow without bound.
    for (auto& b : m_batches) b.data.clear();

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

// Draw — upload and issue draw calls per batch
void ParticleSystem::Draw(const glm::mat4& view, const glm::mat4& projection)
{
    if (m_shader == 0) return;

    bool any = false, anyDistortion = false;
    for (const auto& b : m_batches)
    {
        if (b.data.empty()) continue;
        any = true;
        if (b.key.distortion) anyDistortion = true;
    }
    if (!any) return;

    glDepthMask(GL_FALSE);
    glEnable(GL_BLEND);
    glBindVertexArray(m_quadVAO);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, m_ssbo);

    // Distortion first: it refracts the finished scene, and everything drawn after it (smoke, fire) must sit on top of the refracted pixels.
    if (anyDistortion)
    {
        GLint vp[4];
        glGetIntegerv(GL_VIEWPORT, vp);
        CopySceneColor(vp[2], vp[3]);

        glUseProgram(m_distShader);
        glUniformMatrix4fv(m_dView, 1, GL_FALSE, glm::value_ptr(view));
        glUniformMatrix4fv(m_dProjection, 1, GL_FALSE, glm::value_ptr(projection));
        glUniform2f(m_dViewport, static_cast<float>(vp[2]), static_cast<float>(vp[3]));

        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, m_sceneTex);
        glUniform1i(m_dScene, 1);
        glActiveTexture(GL_TEXTURE0);

        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        for (const auto& b : m_batches)
            if (b.key.distortion) FlushBatch(b);
    }

    glUseProgram(m_shader);
    glUniformMatrix4fv(m_uView, 1, GL_FALSE, glm::value_ptr(view));
    glUniformMatrix4fv(m_uProjection, 1, GL_FALSE, glm::value_ptr(projection));
    glUniform1i(m_uTex, 0);

    // FIX #7 — separate draw calls for each blend mode so smoke and fire
    // can coexist correctly.  Alpha-blended particles are drawn first so
    // additive particles composite on top of them.
    for (int pass = 0; pass < 2; ++pass)
    {
        const bool additive = (pass == 1);
        glBlendFunc(GL_SRC_ALPHA, additive ? GL_ONE : GL_ONE_MINUS_SRC_ALPHA);

        for (const auto& b : m_batches)
        {
            if (b.key.distortion || b.key.additive != additive) continue;

            glUniform1i(m_uAlphaMode, b.key.alphaMode);
            if (b.key.tex != 0)
            {
                glActiveTexture(GL_TEXTURE0);
                glBindTexture(GL_TEXTURE_2D, b.key.tex);
            }
            FlushBatch(b);
        }
    }

    glBindVertexArray(0);
    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);
    glUseProgram(0);
}

// FlushBatch — upload one batch and draw
void ParticleSystem::FlushBatch(const Batch& batch)
{
    if (batch.data.empty()) return;

    EnsureSSBOCapacity(static_cast<int>(batch.data.size()));

    glBindBuffer(GL_SHADER_STORAGE_BUFFER, m_ssbo);
    glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0,
        batch.data.size() * sizeof(GPUParticle),
        batch.data.data());

    glDrawArraysInstanced(GL_TRIANGLES, 0, 6,
        static_cast<GLsizei>(batch.data.size()));
}

ParticleSystem::Batch& ParticleSystem::GetBatch(const BatchKey& key)
{
    for (auto& b : m_batches)
        if (b.key == key) return b;
    m_batches.push_back(Batch{ key, {} });
    return m_batches.back();
}

// Loads (once) the sprite sheet an emitter asked for. Returns 0 on failure so the emitter falls back to the procedural disc.
GLuint ParticleSystem::GetTexture(const std::string& name)
{
    auto it = m_textures.find(name);
    if (it != m_textures.end()) return it->second;

    GLuint tex = 0;
    auto handle = AssetManager::instance().loadAsset<TextureID>(name);
    if (handle)
    {
        tex = handle->value;
    }
    else
    {
        Debug::Error("ParticleSystem") << "Flipbook texture not available (is it listed in the scene's .ntfg?): "
            << name << "\n";
    }
    m_textures[name] = tex;
    return tex;
}

// Copies the currently bound framebuffer's colour into m_sceneTex, (re)allocating it when the viewport size changed.
void ParticleSystem::CopySceneColor(int w, int h)
{
    if (m_sceneTex == 0 || w != m_sceneW || h != m_sceneH)
    {
        if (m_sceneTex == 0) glGenTextures(1, &m_sceneTex);
        glBindTexture(GL_TEXTURE_2D, m_sceneTex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, w, h, 0, GL_RGBA, GL_FLOAT, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        m_sceneW = w;
        m_sceneH = h;
    }
    glBindTexture(GL_TEXTURE_2D, m_sceneTex);
    glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, w, h);
}

void ParticleSystem::SimulateEmitter(ParticleEmitterComponent& e,
    const glm::vec3& emitterWorldPos,
    const glm::vec3& emitterWorldDir,
    float uniformScale,
    float dt)
{
    static const glm::vec3 kGravity(0.0f, -9.81f, 0.0f);

    EnsurePool(e);

    e.elapsedTime += dt;

    const float activeTime = e.elapsedTime - e.startDelay;
    if (e.enabled && activeTime >= 0.0f)
    {
        if (e.burstCount > 0 && !e.burstFired)
        {
            e.burstFired = true;
            for (int i = 0; i < e.burstCount; ++i)
                SpawnParticle(e, emitterWorldPos, emitterWorldDir, uniformScale);
        }

        if (e.looping || activeTime <= e.duration)
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
    }

    // Impact slow motion: particles age/move slower at first, easing back to real time. Emission above keeps using real time so the burst itself is unaffected.
    float simDt = dt;
    if (e.slowMotionDuration > 0.0f && e.elapsedTime < e.slowMotionDuration)
    {
        float k = e.elapsedTime / e.slowMotionDuration;
        k = k * k * (3.0f - 2.0f * k);
        simDt = dt * glm::mix(e.slowMotionScale, 1.0f, k);
    }

    e.aliveCount = 0;

    // Batch is resolved once per emitter: sheet + blend mode decide the draw call.
    const GLuint sheet = (!e.texture.empty() && !e.distortion) ? GetTexture(e.texture) : 0;
    BatchKey key;
    key.tex = sheet;
    key.alphaMode = sheet ? static_cast<int>(e.flipbookAlpha) + 1 : 0;
    key.additive = e.additiveBlend;
    key.distortion = e.distortion;
    auto& staging = GetBatch(key).data;

    const int   flipTotal = (e.flipbookFrames > 0) ? e.flipbookFrames : std::max(1, e.flipbookCols * e.flipbookRows);
    const bool  useRamp = e.colorRampCount > 0;

    for (int idx = 0; idx < static_cast<int>(e.pool.size()); ++idx)
    {
        Particle& p = e.pool[idx];
        if (!p.alive) continue;

        p.age += simDt;
        if (p.age >= p.lifetime)
        {
            p.alive = false;
            // FIX #2 — return slot to free-list in O(1)
            e.freeList.push_back(idx);
            continue;
        }

        p.velocity += kGravity * e.gravityModifier * simDt;

        if (e.drag > 0.0f)
            p.velocity *= std::exp(-e.drag * simDt);

        if (e.turbulenceStrength > 0.0f)
        {
            glm::vec3 turb(
                (RandF() * 2.0f - 1.0f) * e.turbulenceStrength,
                (RandF() * 2.0f - 1.0f) * e.turbulenceStrength,
                (RandF() * 2.0f - 1.0f) * e.turbulenceStrength
            );
            p.velocity += turb * simDt;
        }

        // FIX: SimulationSpace::Local — offset particle by emitter movement
        if (e.simulationSpace == SimulationSpace::Local)
        {
            // Move local-space particles by the emitter's delta since last tick (emitterLastPos updated at function end).
            p.position += emitterWorldPos - e.emitterLastPos;
        }

        p.position += p.velocity * simDt;

        if (e.onUpdate) e.onUpdate(p, simDt);

        ++e.aliveCount;

        float     t = p.age / p.lifetime;
        glm::vec4 color = useRamp ? EvalColorRamp(e, t) : glm::mix(p.colorStart, p.colorEnd, t);
        if (e.fadeOutFraction > 0.0f)
        {
            const float k = glm::clamp((1.0f - t) / e.fadeOutFraction, 0.0f, 1.0f);
            color.a *= k * k * (3.0f - 2.0f * k);
        }
        color.r *= e.emissiveScale;
        color.g *= e.emissiveScale;
        color.b *= e.emissiveScale;
        float     size = glm::mix(p.sizeStart, p.sizeEnd, t);

        glm::vec4 flip(0.0f);
        if (sheet)
        {
            float frame = 0.0f;
            switch (e.flipbookPlayback)
            {
            case FlipbookPlayback::OverLifetime: frame = t * static_cast<float>(flipTotal - 1); break;
            case FlipbookPlayback::FixedFps:     frame = std::fmod(p.age * e.flipbookFps, static_cast<float>(flipTotal)); break;
            case FlipbookPlayback::RandomFrame:  frame = std::floor(p.seed * static_cast<float>(flipTotal)); break;
            }
            if (!e.flipbookBlend) frame = std::floor(frame);
            flip = glm::vec4(static_cast<float>(e.flipbookCols), static_cast<float>(e.flipbookRows),
                frame, static_cast<float>(flipTotal));
        }

        // Distortion rings stay axis-aligned: their displacement direction is computed in screen space.
        const float rotation = e.distortion ? 0.0f
            : (e.randomRotation ? p.seed * 6.2831853f : 0.0f) + p.spin * p.age;
        // params.y carries the noise amount for sprites and the displacement strength for distortion rings.
        const float paramY = e.distortion ? e.distortionStrength : e.noiseAmount;

        staging.push_back({
            glm::vec4(p.position, size),
            color,
            glm::vec4(p.velocity, p.seed),
            glm::vec4(e.stretch, paramY, rotation, t),
            flip
        });
    }

    // Record emitter position so Local-space particles can track it next frame
    e.emitterLastPos = emitterWorldPos;

    // Mark done once a non-looping emitter has passed its duration and every
    // particle has died.  Game logic can poll e.done to remove/recycle the entity.
    if (!e.looping && !e.done
        && activeTime > e.duration
        && e.aliveCount == 0)
    {
        e.done = true;
    }
}

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

    float lifetime = e.startLifetime * e.speedScale;
    if (e.lifetimeVariance > 0.0f)
        lifetime += (RandF() * 2.0f - 1.0f) * e.lifetimeVariance;
    lifetime = std::max(0.05f, lifetime);

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
    float sizeJitter = (e.sizeVariance > 0.0f) ? (RandF() * 2.0f - 1.0f) * e.sizeVariance : 0.0f;
    slot.sizeStart = std::max(0.0f, e.startSize + sizeJitter) * uniformScale;
    slot.sizeEnd = std::max(0.0f, e.endSize + sizeJitter) * uniformScale;
    // FIX: removed the misleading emitterWorldPos argument from SampleSpawnPosition
    slot.position = emitterWorldPos + (SampleSpawnPosition(e) + e.spawnOffset) * uniformScale;
    slot.velocity = spawnDir * speed;
    slot.seed = RandF();
    slot.spin = (e.spinSpeed > 0.0f) ? (RandF() * 2.0f - 1.0f) * e.spinSpeed : 0.0f;
}

// SampleSpawnPosition — returns a local-space offset (before uniformScale).
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

// SampleSpawnVelocity — outConeT is the normalised [0,1] deviation from the cone axis (0=centre, 1=edge); only meaningful for EmitterShape::Cone.
glm::vec3 ParticleSystem::SampleSpawnVelocity(const ParticleEmitterComponent& e,
    const glm::vec3& emitterWorldDir,
    float& outConeT)
{
    outConeT = 0.0f;

    switch (e.shape)
    {
    case EmitterShape::Cone:
    {
        // Uniform solid-angle sample within the cone: sample cos(angle) uniformly in [cos(halfAngle), 1], not the angle itself (which biases toward the axis).
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

// EnsurePool — initialises or resizes the particle pool and free-list.
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

void ParticleSystem::CompileShaders()
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

    auto buildProgram = [&](const char* fragSrc) -> GLuint {
        GLuint vert = compileStage(GL_VERTEX_SHADER, kParticleVert);
        GLuint frag = compileStage(GL_FRAGMENT_SHADER, fragSrc);

        GLuint program = glCreateProgram();
        glAttachShader(program, vert);
        glAttachShader(program, frag);
        glLinkProgram(program);

        GLint ok = 0;
        glGetProgramiv(program, GL_LINK_STATUS, &ok);
        if (!ok) {
            char log[512];
            glGetProgramInfoLog(program, 512, nullptr, log);
            Debug::Error("ParticleSystem::Program") << log << "\n";
        }

        glDetachShader(program, vert); glDeleteShader(vert);
        glDetachShader(program, frag); glDeleteShader(frag);
        return program;
        };

    m_shader = buildProgram(kParticleFrag);
    m_distShader = buildProgram(kDistortionFrag);
}

void ParticleSystem::InitQuadVAO()
{
    // No vertex data — positions built entirely in the vertex shader.
    glGenVertexArrays(1, &m_quadVAO);
}

ParticleSystem::~ParticleSystem()
{
    for (const auto& [name, tex] : m_textures)
        if (tex != 0)
            AssetManager::instance().unloadAsset<TextureID>(name);

    if (m_sceneTex) glDeleteTextures(1, &m_sceneTex);
    glDeleteProgram(m_shader);
    glDeleteProgram(m_distShader);
    glDeleteVertexArrays(1, &m_quadVAO);
    glDeleteBuffers(1, &m_ssbo);
}
