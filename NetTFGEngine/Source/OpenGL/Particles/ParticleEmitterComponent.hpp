#ifndef PARTICLE_EMITTER_COMPONENT_HPP
#define PARTICLE_EMITTER_COMPONENT_HPP

#include <glm/glm.hpp>
#include <functional>
#include <vector>

#include "ecs/ecs.hpp"

// Individual particle state — CPU side only; the GPU sees a packed GPUParticle uploaded each frame.
struct Particle {
    glm::vec3 position = glm::vec3(0.0f);
    glm::vec3 velocity = glm::vec3(0.0f);
    glm::vec4 colorStart = glm::vec4(1.0f);
    glm::vec4 colorEnd = glm::vec4(1.0f, 1.0f, 1.0f, 0.0f);
    float     sizeStart = 0.1f;
    float     sizeEnd = 0.0f;
    float     lifetime = 1.0f;  // total lifetime in seconds
    float     age = 0.0f;  // elapsed time in seconds
    bool      alive = false;
};

// GPU-side layout — matches the SSBO binding in the particle billboard shader (std430).
struct GPUParticle {
    glm::vec4 positionSize;  // xyz = world pos, w = size
    glm::vec4 color;         // rgba — lerped from colorStart→colorEnd
};

enum class EmitterShape {
    Point,   // all particles start at the emitter origin
    Sphere,  // random point inside a sphere of radius `shapeRadius`
    Cone,    // random direction within `shapeConeAngle` radians
};

enum class SimulationSpace {
    World,   // particles keep world-space positions after spawn
    Local,   // particles move with the emitter's Transform
};

// Pure data component — no virtual methods, no heap allocation except the particle pool (resized once on first spawn). Create via ParticlePresets::* or configure manually.
struct ParticleEmitterComponent : public IComponent {
    // --- Emission ------------------------------------------------
    bool  enabled = true;
    bool  looping = true;
    float duration = 5.0f;   // seconds; ignored when looping
    float emissionRate = 20.0f;  // particles / second

    // --- Particle lifetime / motion -----------------------------
    float startLifetime = 1.0f;  // seconds
    float startSpeed = 1.0f;  // world units / second along spawn direction
    float startSize = 0.1f;  // world units
    float gravityModifier = 0.0f;  // multiplier on (0,-9.81,0)

    // --- Colour over lifetime -----------------------------------
    glm::vec4 startColor = glm::vec4(1.0f);
    glm::vec4 endColor = glm::vec4(1.0f, 1.0f, 1.0f, 0.0f);

    // --- Size over lifetime -------------------------------------
    float endSize = 0.0f;  // lerps startSize → endSize

    // --- Shape --------------------------------------------------
    EmitterShape shape = EmitterShape::Point;
    float        shapeRadius = 0.5f;   // Sphere radius
    float        shapeConeAngle = 0.3f;   // Cone half-angle in radians (~17°)

    // --- Simulation space ---------------------------------------
    SimulationSpace simulationSpace = SimulationSpace::World;

    // --- Limits -------------------------------------------------
    int maxParticles = 100;

    // --- Realism / variation: per-particle jitter on lifetime/emission/speed, turbulence impulse, and a runtime speedScale multiplier ---
    float lifetimeVariance = 0.0f;
    float emissionVariance = 0.0f;
    float speedVariance = 0.0f;
    float turbulenceStrength = 0.0f;
    float speedScale = 1.0f;

    // --- Blending -----------------------------------------------
    //  true  → additive blend (GL_SRC_ALPHA, GL_ONE)       — fire, sparks, magic
    //  false → standard alpha blend (GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA) — smoke, rain
    bool additiveBlend = true;

    // --- Color temperature (cone emitters only): particles near the cone centre lerp toward white-hot, based on angular deviation from the emitter axis ---
    bool colorTemperature = false;

    // --- Optional custom update hook: (particle, deltaTime), called once per alive particle after default physics integration; nullptr for default behaviour ---
    std::function<void(Particle&, float)> onUpdate = nullptr;

    // --- Runtime state (managed by ParticleSystem) ---
    std::vector<Particle> pool;           // particle pool, size = maxParticles
    std::vector<int>      freeList;       // indices of dead slots — O(1) spawn
    float     emissionAccum = 0.0f;     // fractional-particle accumulator
    float     elapsedTime = 0.0f;     // total emitter age in seconds
    int       aliveCount = 0;        // for stats / culling
    bool      done = false;    // true once a non-looping emitter has finished and all particles died; poll to know when to remove/recycle
    glm::vec3 emitterLastPos = glm::vec3(0.0f); // previous world position, used by SimulationSpace::Local
};

#endif // PARTICLE_EMITTER_COMPONENT_HPP