#ifndef PARTICLE_PRESETS_HPP
#define PARTICLE_PRESETS_HPP

#include "ParticleEmitterComponent.hpp"

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
        return e;
    }

} // namespace ParticlePresets

#endif // PARTICLE_PRESETS_HPP