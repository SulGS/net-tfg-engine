#ifndef PARTICLE_EMITTER_COMPONENT_HPP
#define PARTICLE_EMITTER_COMPONENT_HPP

#include <glm/glm.hpp>
#include <functional>
#include <vector>
#include <string>

#include "ecs/ecs.hpp"

// -------------------------------------------------------
//  Individual particle state — CPU side only.
//  The GPU sees a packed GPUParticle uploaded each frame.
// -------------------------------------------------------
struct Particle {
    glm::vec3 position = glm::vec3(0.0f);
    glm::vec3 velocity = glm::vec3(0.0f);
    glm::vec4 colorStart = glm::vec4(1.0f);
    glm::vec4 colorEnd = glm::vec4(1.0f, 1.0f, 1.0f, 0.0f);
    float     sizeStart = 0.1f;
    float     sizeEnd = 0.0f;
    float     lifetime = 1.0f;   // total lifetime in seconds
    float     age = 0.0f;   // elapsed time in seconds
    bool      alive = false;
};

// -------------------------------------------------------
//  GPU-side layout — matches the SSBO binding in the
//  particle billboard shader (std430).
// -------------------------------------------------------
struct GPUParticle {
    glm::vec4 positionSize;  // xyz = world pos, w = size
    glm::vec4 color;         // rgba — lerped from colorStart→colorEnd
};

// -------------------------------------------------------
//  Spawn shape
// -------------------------------------------------------
enum class EmitterShape {
    Point,   // all particles start at the emitter origin
    Sphere,  // random point inside a sphere of radius `shapeRadius`
    Cone,    // random direction within `shapeConeAngle` radians
};

// -------------------------------------------------------
//  Simulation space
// -------------------------------------------------------
enum class SimulationSpace {
    World,   // particles keep world-space position after spawn
    Local,   // particles move with the emitter's Transform
};

// -------------------------------------------------------
//  ParticleEmitterComponent
//
//  Pure data component — no virtual methods, no heap
//  allocation except for the particle pool (resized once
//  in ParticleSystem::OnEmitterAdded).
//
//  Create via ParticlePresets::* or configure manually.
// -------------------------------------------------------
struct ParticleEmitterComponent : public IComponent {
    // --- Emission ------------------------------------------------
    bool  enabled = true;
    bool  looping = true;
    float duration = 5.0f;  // seconds; ignored when looping
    float emissionRate = 20.0f; // particles / second

    // --- Particle lifetime / motion -----------------------------
    float startLifetime = 1.0f;   // seconds
    float startSpeed = 1.0f;   // world units / second along spawn direction
    float startSize = 0.1f;   // world units
    float gravityModifier = 0.0f; // multiplier on (0,-9.81,0)

    // --- Colour over lifetime -----------------------------------
    glm::vec4 startColor = glm::vec4(1.0f);
    glm::vec4 endColor = glm::vec4(1.0f, 1.0f, 1.0f, 0.0f);

    // --- Size over lifetime -------------------------------------
    float endSize = 0.0f;   // lerps startSize → endSize

    // --- Shape --------------------------------------------------
    EmitterShape shape = EmitterShape::Point;
    float        shapeRadius = 0.5f; // Sphere radius
    float        shapeConeAngle = 0.3f; // Cone half-angle in radians (~17°)

    // --- Simulation space ---------------------------------------
    SimulationSpace simulationSpace = SimulationSpace::World;

    // --- Limits -------------------------------------------------
    int maxParticles = 100;

    // --- Realism / variation -----------------------------------
    //  lifetimeVariance   : lifetime = startLifetime +/- rand * variance
    //  emissionVariance   : emissionRate jitters +/- rand * variance per frame
    //  speedVariance      : speed = startSpeed +/- rand * variance per particle
    //  turbulenceStrength : random impulse added to velocity each frame
    //  speedScale         : runtime multiplier — set each frame by game logic
    //                       scales both emissionRate and startLifetime
    float lifetimeVariance = 0.0f;
    float emissionVariance = 0.0f;
    float speedVariance = 0.0f;
    float turbulenceStrength = 0.0f;
    float speedScale = 1.0f;
    bool  colorTemperature = false; // if true, cone-center particles lerp toward white-hot

    // --- Optional custom update hook ----------------------------
    //  Called once per alive particle per frame, after the default
    //  physics integration.  Set to nullptr to use default behaviour.
    //  Signature: (particle, deltaTime)
    std::function<void(Particle&, float)> onUpdate = nullptr;

    // --- Runtime state (managed by ParticleSystem) --------------
    std::vector<Particle> pool;       // particle pool, size = maxParticles
    float emissionAccum = 0.0f;      // fractional-particle accumulator
    float elapsedTime = 0.0f;      // total emitter age
    int   aliveCount = 0;         // for stats / culling
};

#endif // PARTICLE_EMITTER_COMPONENT_HPP