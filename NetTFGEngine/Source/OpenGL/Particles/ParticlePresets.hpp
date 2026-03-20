#ifndef PARTICLE_PRESETS_HPP
#define PARTICLE_PRESETS_HPP

#include "ParticleEmitterComponent.hpp"

// -------------------------------------------------------
//  ParticlePresets
//
//  Factory functions that return fully-configured
//  ParticleEmitterComponents.  Start from a preset and
//  tweak individual fields as needed — they are plain
//  structs, so every member is assignable directly.
//
//  Example:
//      auto emitter = ParticlePresets::Fire();
//      emitter.emissionRate = 80.0f;   // tweak
//      entity.Add<ParticleEmitterComponent>(emitter);
// -------------------------------------------------------
namespace ParticlePresets
{
    // -------------------------------------------------------
    //  Fire — fast upward burst, orange→red→transparent,
    //  medium lifetime, slight gravity lift (negative modifier).
    // -------------------------------------------------------
    inline ParticleEmitterComponent Fire()
    {
        ParticleEmitterComponent e;
        e.emissionRate = 60.0f;
        e.startLifetime = 1.2f;
        e.startSpeed = 1.5f;
        e.startSize = 0.25f;
        e.endSize = 0.05f;
        e.gravityModifier = -0.3f;          // slight upward drift
        e.startColor = glm::vec4(1.0f, 0.55f, 0.0f, 1.0f);  // orange
        e.endColor = glm::vec4(0.8f, 0.1f, 0.0f, 0.0f);  // red, fade out
        e.shape = EmitterShape::Cone;
        e.shapeConeAngle = 0.25f;
        e.maxParticles = 200;
        e.looping = true;
        return e;
    }

    // -------------------------------------------------------
    //  Smoke — slow rise, large grey billboards, long life.
    // -------------------------------------------------------
    inline ParticleEmitterComponent Smoke()
    {
        ParticleEmitterComponent e;
        e.emissionRate = 15.0f;
        e.startLifetime = 4.0f;
        e.startSpeed = 0.4f;
        e.startSize = 0.3f;
        e.endSize = 1.2f;
        e.gravityModifier = -0.05f;
        e.startColor = glm::vec4(0.4f, 0.4f, 0.4f, 0.6f);
        e.endColor = glm::vec4(0.6f, 0.6f, 0.6f, 0.0f);
        e.shape = EmitterShape::Sphere;
        e.shapeRadius = 0.15f;
        e.maxParticles = 120;
        e.looping = true;
        return e;
    }

    // -------------------------------------------------------
    //  Sparks — fast, short-lived, bright yellow/white,
    //  affected by gravity, spawned in all directions.
    // -------------------------------------------------------
    inline ParticleEmitterComponent Sparks()
    {
        ParticleEmitterComponent e;
        e.emissionRate = 0.0f;    // burst-style: set manually per event
        e.startLifetime = 0.6f;
        e.startSpeed = 4.0f;
        e.startSize = 0.05f;
        e.endSize = 0.01f;
        e.gravityModifier = 1.0f;
        e.startColor = glm::vec4(1.0f, 0.95f, 0.4f, 1.0f);
        e.endColor = glm::vec4(1.0f, 0.3f, 0.0f, 0.0f);
        e.shape = EmitterShape::Sphere;
        e.shapeRadius = 0.05f;
        e.maxParticles = 80;
        e.looping = false;
        e.duration = 0.1f;
        return e;
    }

    // -------------------------------------------------------
    //  Rain — straight down, thin, fast, high volume.
    // -------------------------------------------------------
    inline ParticleEmitterComponent Rain()
    {
        ParticleEmitterComponent e;
        e.emissionRate = 200.0f;
        e.startLifetime = 1.5f;
        e.startSpeed = 8.0f;
        e.startSize = 0.03f;
        e.endSize = 0.03f;
        e.gravityModifier = 1.0f;
        e.startColor = glm::vec4(0.7f, 0.85f, 1.0f, 0.7f);
        e.endColor = glm::vec4(0.7f, 0.85f, 1.0f, 0.0f);
        e.shape = EmitterShape::Sphere;
        e.shapeRadius = 5.0f;   // wide spawn volume overhead
        e.simulationSpace = SimulationSpace::World;
        e.maxParticles = 600;
        e.looping = true;
        return e;
    }

    // -------------------------------------------------------
    //  Magic — slow, swirling colour, glowing, medium life.
    // -------------------------------------------------------
    inline ParticleEmitterComponent Magic()
    {
        ParticleEmitterComponent e;
        e.emissionRate = 30.0f;
        e.startLifetime = 2.0f;
        e.startSpeed = 0.6f;
        e.startSize = 0.12f;
        e.endSize = 0.0f;
        e.gravityModifier = -0.1f;
        e.startColor = glm::vec4(0.4f, 0.2f, 1.0f, 1.0f);  // purple
        e.endColor = glm::vec4(0.0f, 0.8f, 1.0f, 0.0f);  // cyan, fade
        e.shape = EmitterShape::Sphere;
        e.shapeRadius = 0.3f;
        e.maxParticles = 150;
        e.looping = true;
        return e;
    }

    // -------------------------------------------------------
    //  SpaceshipThruster — hot plasma exhaust jet.
    //  Attach to an entity whose -Z axis points backward
    //  (i.e. the exhaust nozzle direction).
    //
    //  Tuning tips:
    //    emissionRate  — higher = more thrust visually
    //    startSpeed    — matches the perceived thrust power
    //    shapeConeAngle — tighter (< 0.1) = focused jet,
    //                     wider  (> 0.3) = damaged/sputtering
    // -------------------------------------------------------
    inline ParticleEmitterComponent SpaceshipThruster()
    {
        ParticleEmitterComponent e;
        e.emissionRate = 120.0f;
        e.startLifetime = 0.4f;        // plasma disperses fast
        e.startSpeed = 6.0f;        // strong focused jet
        e.startSize = 0.08f;       // tight core
        e.endSize = 0.35f;       // expands as it disperses
        e.gravityModifier = 0.0f;        // space — no gravity
        e.startColor = glm::vec4(0.6f, 0.85f, 1.0f, 1.0f);  // hot blue-white core
        e.endColor = glm::vec4(0.1f, 0.3f, 0.8f, 0.0f);  // deep blue, fade out
        e.shape = EmitterShape::Cone;
        e.shapeConeAngle = 0.08f;       // very focused — tighten further for a clean thruster
        e.simulationSpace = SimulationSpace::World;
        e.maxParticles = 150;
        e.looping = true;
        return e;
    }

} // namespace ParticlePresets

#endif // PARTICLE_PRESETS_HPP