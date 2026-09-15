#ifndef PARTICLE_PRESETS_HPP
#define PARTICLE_PRESETS_HPP

#include "ParticleEmitterComponent.hpp"
#include <vector>

namespace ParticlePresets
{
    inline ParticleEmitterComponent Fire()
    {
        ParticleEmitterComponent e;
        e.emissionRate = 60.0f;
        e.startLifetime = 1.2f;
        e.startSpeed = 1.5f;
        e.startSize = 0.25f;
        e.endSize = 0.05f;
        e.gravityModifier = -0.3f;
        e.startColor = glm::vec4(1.0f, 0.55f, 0.0f, 1.0f);
        e.endColor = glm::vec4(0.8f, 0.1f, 0.0f, 0.0f);
        e.shape = EmitterShape::Cone;
        e.shapeConeAngle = 0.25f;
        e.maxParticles = 200;
        e.looping = true;
        e.lifetimeVariance = 0.3f;
        e.emissionVariance = 20.0f;
        e.speedVariance = 0.5f;
        e.turbulenceStrength = 0.6f;
        e.additiveBlend = true;   // fire glows — additive
        return e;
    }

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
        e.lifetimeVariance = 1.0f;
        e.emissionVariance = 5.0f;
        e.speedVariance = 0.15f;
        e.turbulenceStrength = 0.08f;
        e.additiveBlend = false;  // smoke occludes — standard alpha
        return e;
    }

    inline ParticleEmitterComponent Sparks()
    {
        ParticleEmitterComponent e;
        e.emissionRate = 0.0f;
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
        e.lifetimeVariance = 0.2f;
        e.emissionVariance = 0.0f;
        e.speedVariance = 2.5f;
        e.turbulenceStrength = 0.0f;
        e.additiveBlend = true;   // sparks glow — additive
        return e;
    }

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
        e.shapeRadius = 5.0f;
        e.simulationSpace = SimulationSpace::World;
        e.maxParticles = 600;
        e.looping = true;
        e.lifetimeVariance = 0.3f;
        e.emissionVariance = 40.0f;
        e.speedVariance = 1.5f;
        e.turbulenceStrength = 0.15f;
        e.additiveBlend = false;  // rain is translucent, not emissive
        return e;
    }

    inline ParticleEmitterComponent Magic()
    {
        ParticleEmitterComponent e;
        e.emissionRate = 30.0f;
        e.startLifetime = 2.0f;
        e.startSpeed = 0.6f;
        e.startSize = 0.12f;
        e.endSize = 0.0f;
        e.gravityModifier = -0.1f;
        e.startColor = glm::vec4(0.4f, 0.2f, 1.0f, 1.0f);
        e.endColor = glm::vec4(0.0f, 0.8f, 1.0f, 0.0f);
        e.shape = EmitterShape::Sphere;
        e.shapeRadius = 0.3f;
        e.maxParticles = 150;
        e.looping = true;
        e.lifetimeVariance = 0.6f;
        e.emissionVariance = 10.0f;
        e.speedVariance = 0.3f;
        e.turbulenceStrength = 0.5f;
        e.additiveBlend = true;   // magic glows — additive
        return e;
    }

    inline ParticleEmitterComponent SpaceshipThruster()
    {
        ParticleEmitterComponent e;
        e.emissionRate = 180.0f;
        e.startLifetime = 0.35f;
        e.startSpeed = 7.0f;
        e.startSize = 0.06f;
        e.endSize = 0.3f;
        e.gravityModifier = 0.0f;
        e.startColor = glm::vec4(0.6f, 0.85f, 1.0f, 1.0f);
        e.endColor = glm::vec4(0.1f, 0.3f, 0.8f, 0.0f);
        e.shape = EmitterShape::Cone;
        e.shapeConeAngle = 0.08f;
        e.simulationSpace = SimulationSpace::World;
        e.maxParticles = 200;
        e.looping = true;
        e.lifetimeVariance = 0.08f;
        e.emissionVariance = 30.0f;
        e.speedVariance = 1.5f;
        e.turbulenceStrength = 0.3f;
        e.colorTemperature = true;
        e.additiveBlend = true;   // thruster exhaust glows — additive
        return e;
    }

    // Realistic large explosion, four one-shot layers spawned together at the same position: Flash, Fireball, Debris, Smoke (or use MakeExplosion()); poll each emitter.done before destroying.

    // Layer 1 — blinding flash (~0.3 s)
    // Extremely bright, large particles that bloom and vanish.
    // Additive so overlapping particles saturate to white.
    inline ParticleEmitterComponent ExplosionFlash()
    {
        ParticleEmitterComponent e;
        e.looping = false;
        e.duration = 0.05f;    // near-instant burst
        e.emissionRate = 2000.0f;  // ~100 particles in the window
        e.emissionVariance = 0.0f;

        e.startLifetime = 0.28f;
        e.lifetimeVariance = 0.06f;

        e.startSpeed = 12.0f;       // fast outward bloom
        e.speedVariance = 6.0f;

        e.startSize = 1.8f;            // large — fills the blast radius
        e.endSize = 0.0f;

        e.gravityModifier = 0.0f;      // flash ignores gravity

        e.startColor = glm::vec4(1.0f, 0.92f, 0.75f, 1.0f);  // near-white hot
        e.endColor = glm::vec4(1.0f, 0.4f, 0.0f, 0.0f);  // orange fade-out

        e.shape = EmitterShape::Sphere;
        e.shapeRadius = 0.3f;

        e.maxParticles = 200;
        e.turbulenceStrength = 0.0f;   // clean radial — no turbulence on the flash
        e.simulationSpace = SimulationSpace::World;
        e.additiveBlend = true;
        return e;
    }

    // Layer 2 — rising fireball (~2.5 s): large slow particles with negative gravity, standard alpha blend so the cloud occludes properly.
    inline ParticleEmitterComponent ExplosionFireball()
    {
        ParticleEmitterComponent e;
        e.looping = false;
        e.duration = 0.35f;    // sustained burst, not instant
        e.emissionRate = 120.0f;
        e.emissionVariance = 30.0f;

        e.startLifetime = 2.4f;
        e.lifetimeVariance = 0.7f;

        e.startSpeed = 4.5f;
        e.speedVariance = 2.5f;

        e.startSize = 2.5f;            // very large billowing puffs
        e.endSize = 5.5f;            // expands as the gas cools and spreads

        e.gravityModifier = -0.25f;    // hot gas rises

        // Deep orange-red core bleeds to grey-brown as it cools
        e.startColor = glm::vec4(0.9f, 0.35f, 0.02f, 0.9f);
        e.endColor = glm::vec4(0.18f, 0.14f, 0.12f, 0.0f);

        e.shape = EmitterShape::Sphere;
        e.shapeRadius = 1.5f;          // wide spawn radius — not a point source

        e.maxParticles = 200;
        e.turbulenceStrength = 1.2f;   // heavy turbulence for boiling fireball look
        e.simulationSpace = SimulationSpace::World;
        e.additiveBlend = false;  // occludes — standard alpha
        return e;
    }

    // Layer 3 — debris chunks (~1.8 s)
    // Fast small particles with strong gravity — shrapnel and
    // dirt clods that arc outward and fall back to the ground.
    inline ParticleEmitterComponent ExplosionDebris()
    {
        ParticleEmitterComponent e;
        e.looping = false;
        e.duration = 0.12f;    // sharp burst
        e.emissionRate = 800.0f;
        e.emissionVariance = 0.0f;

        e.startLifetime = 1.7f;
        e.lifetimeVariance = 0.6f;

        e.startSpeed = 18.0f;       // fast — chunks fly outward
        e.speedVariance = 9.0f;

        e.startSize = 0.25f;           // small — individual chunks/rocks
        e.endSize = 0.08f;

        e.gravityModifier = 1.8f;      // strong gravity — debris falls hard

        // Dark grey-brown earth and rock tones
        e.startColor = glm::vec4(0.35f, 0.28f, 0.18f, 1.0f);
        e.endColor = glm::vec4(0.2f, 0.16f, 0.1f, 0.0f);

        e.shape = EmitterShape::Cone;
        e.shapeConeAngle = 1.1f;       // wide cone — debris sprays outward and up

        e.maxParticles = 300;
        e.turbulenceStrength = 0.15f;  // slight wobble simulates irregular shapes
        e.simulationSpace = SimulationSpace::World;
        e.additiveBlend = false;
        return e;
    }

    // Layer 4 — smoke column (~8 s)
    // Large slow-expanding dark smoke that rises long after the
    // fireball has faded.  The most visible layer at distance.
    inline ParticleEmitterComponent ExplosionSmoke()
    {
        ParticleEmitterComponent e;
        e.looping = false;
        e.duration = 1.8f;     // sustained — keeps feeding the column
        e.emissionRate = 35.0f;
        e.emissionVariance = 10.0f;

        e.startLifetime = 7.5f;
        e.lifetimeVariance = 1.5f;

        e.startSpeed = 1.8f;
        e.speedVariance = 0.8f;

        e.startSize = 3.0f;            // large puffs
        e.endSize = 9.0f;            // expand massively as smoke rises

        e.gravityModifier = -0.12f;    // smoke rises slowly

        // Near-black smoke fades to transparent grey
        e.startColor = glm::vec4(0.1f, 0.09f, 0.08f, 0.85f);
        e.endColor = glm::vec4(0.35f, 0.33f, 0.31f, 0.0f);

        e.shape = EmitterShape::Sphere;
        e.shapeRadius = 2.0f;

        e.maxParticles = 400;
        e.turbulenceStrength = 0.35f;  // slow billowing motion
        e.simulationSpace = SimulationSpace::World;
        e.additiveBlend = false;  // smoke occludes sky
        return e;
    }

    // Convenience wrapper: returns all four explosion layers as a vector, ready to attach to entities at the same world position.
    inline std::vector<ParticleEmitterComponent> MakeExplosion()
    {
        return {
            ExplosionFlash(),
            ExplosionFireball(),
            ExplosionDebris(),
            ExplosionSmoke(),
        };
    }

    // Non-repeating template: emits steadily for `duration` seconds then stops; variance/turbulence left at zero for callers to configure. Poll emitter.done to recycle.
    inline ParticleEmitterComponent SingleSweep()
    {
        ParticleEmitterComponent e;
        e.looping = false;
        e.duration = 1.0f;   // override per use-case
        e.emissionRate = 30.0f;  // override per use-case
        e.startLifetime = 1.5f;
        e.startSpeed = 1.0f;
        e.startSize = 0.1f;
        e.endSize = 0.0f;
        e.gravityModifier = 0.0f;
        e.startColor = glm::vec4(1.0f);
        e.endColor = glm::vec4(1.0f, 1.0f, 1.0f, 0.0f);
        e.shape = EmitterShape::Sphere;
        e.shapeRadius = 0.1f;
        e.maxParticles = 128;
        e.additiveBlend = true;
        // done will become true automatically once duration elapses
        // and aliveCount reaches zero.
        return e;
    }

    // Bright single-burst energy explosion (no fire): core flash, energy ring and afterglow approximated via variance ranges in one shared pool; one-shot, poll emitter.done to clean up.
    inline ParticleEmitterComponent SciFiExplosion()
    {
        ParticleEmitterComponent e;

        // Emission — fires a dense burst in under one tick at 60 fps
        e.looping = false;
        e.duration = 0.07f;   // burst window in seconds
        e.emissionRate = 1800.0f; // spawns ~126 particles in that window
        e.emissionVariance = 0.0f;  // no flicker — deterministic burst

        // Lifetime — wide variance gives the layered feel
        e.startLifetime = 0.9f;
        e.lifetimeVariance = 0.55f; // range ≈ [0.35, 1.45] s

        // Speed — high base + large variance spans flash through afterglow
        e.startSpeed = 6.0f;
        e.speedVariance = 5.5f;     // range ≈ [0.5, 11.5] wu/s

        // Size — starts large, collapses to a point
        e.startSize = 0.18f;
        e.endSize = 0.0f;

        // Gravity — slight upward drift (energy rising), not falling debris
        e.gravityModifier = -0.08f;

        // Colour — white-hot core bleeds to electric cyan then deep blue
        e.startColor = glm::vec4(0.85f, 0.97f, 1.0f, 1.0f);  // near-white cyan
        e.endColor = glm::vec4(0.0f, 0.35f, 1.0f, 0.0f);  // deep blue, faded

        // Shape — omnidirectional sphere burst, tight radius (origin flash)
        e.shape = EmitterShape::Sphere;
        e.shapeRadius = 0.04f;

        // Pool — enough headroom for the full burst plus lifetime overlap
        e.maxParticles = 512;

        // Slight turbulence breaks the perfect sphere into organic tendrils
        e.turbulenceStrength = 0.25f;

        // Simulation space — world-space so particles don't chase the entity
        e.simulationSpace = SimulationSpace::World;

        // Rendering — additive so overlapping particles bloom bright white
        e.additiveBlend = true;

        return e;
    }

} // namespace ParticlePresets

#endif // PARTICLE_PRESETS_HPP