#ifndef PARTICLE_PRESETS_HPP
#define PARTICLE_PRESETS_HPP

#include "ParticleEmitterComponent.hpp"
#include <initializer_list>
#include <string>
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

    // ---------------------------------------------------------------------------------------------------------------------------------
    // Spaceship explosion
    //
    // Seven one-shot layers (flash, body, sparks, fine debris, secondary blasts, refraction ring; the body is a flipbook or a procedural fireball + soot pair) spawned together at the same position (use MakeExplosion()); poll each emitter.done before destroying.
    // Vacuum physics: no gravity and no rising smoke column. What makes it read as a real blast:
    //   - every layer decelerates quickly (drag) after the initial burst, like an expanding pressure front, while metal debris barely slows;
    //   - colours are HDR (rgb well above 1) along a blackbody-like ramp, so bloom does the glow: white-blue core -> yellow -> orange -> deep red -> soot;
    //   - the first fraction of a second plays in slow motion, so the front "hangs" before rushing outward;
    //   - a screen-space distortion ring refracts the scene behind the blast front.
    // Layers are tuned for an emitter Transform scale of 2; MakeExplosion(scale) rescales the speeds to match other scales.
    // ---------------------------------------------------------------------------------------------------------------------------------

    // Sets a colour-over-lifetime ramp (max ParticleEmitterComponent::kMaxColorStops stops, sorted by t). rgb may exceed 1.
    inline void SetColorRamp(ParticleEmitterComponent& e, std::initializer_list<ColorStop> stops)
    {
        int n = 0;
        for (const ColorStop& s : stops)
        {
            if (n >= ParticleEmitterComponent::kMaxColorStops) break;
            e.colorRamp[n++] = s;
        }
        e.colorRampCount = n;
        if (n > 0)
        {
            e.startColor = e.colorRamp[0].color;
            e.endColor = e.colorRamp[n - 1].color;
        }
    }

    // Generic flipbook (sprite-sheet) settings. texture is the asset name exactly as packed in the scene's .ntfg; empty = keep the procedural sprite.
    // Frames are read left-to-right, top-to-bottom (frame 0 = top-left).
    struct FlipbookSettings
    {
        std::string      texture;
        int              cols = 1;
        int              rows = 1;
        int              frames = 0;                                   // 0 = cols * rows
        FlipbookPlayback playback = FlipbookPlayback::OverLifetime;
        float            fps = 24.0f;                                  // FixedFps only
        bool             blend = true;                                 // cross-fade adjacent frames
        FlipbookAlpha    alpha = FlipbookAlpha::TextureAlpha;
        // Where the animation's visual centre sits relative to the cell centre, as a fraction of the frame size (x right, y up). Sheets whose explosion is born off-centre set this so it starts on the emitter.
        glm::vec2        centerOffset = glm::vec2(0.0f);
    };

    // Turns any emitter into a flipbook emitter. No-op when fb.texture is empty. The tint/HDR ramp of the emitter still multiplies the sheet.
    inline void ApplyFlipbook(ParticleEmitterComponent& e, const FlipbookSettings& fb)
    {
        if (fb.texture.empty()) return;
        e.texture = fb.texture;
        e.flipbookCols = fb.cols;
        e.flipbookRows = fb.rows;
        e.flipbookFrames = fb.frames;
        e.flipbookPlayback = fb.playback;
        e.flipbookFps = fb.fps;
        e.flipbookBlend = fb.blend;
        e.flipbookAlpha = fb.alpha;
        e.noiseAmount = 0.0f;   // the sheet already carries the detail
    }

    // Layer 1 — blinding flash (~0.2 s): overlapping additive HDR particles saturate to white, then cool to orange.
    inline ParticleEmitterComponent ExplosionFlash()
    {
        ParticleEmitterComponent e;
        e.looping = false;
        e.duration = 0.04f;
        e.emissionRate = 800.0f;   // ~32 particles in the window
        e.emissionVariance = 0.0f;

        e.startLifetime = 0.2f;
        e.lifetimeVariance = 0.06f;

        e.startSpeed = 3.0f;
        e.speedVariance = 2.0f;
        e.drag = 4.0f;             // the flash stays at the origin instead of drifting away

        e.startSize = 0.8f;
        e.sizeVariance = 0.3f;
        e.endSize = 0.3f;

        e.gravityModifier = 0.0f;

        SetColorRamp(e, {
            { 0.00f, glm::vec4(4.0f, 3.4f, 2.4f, 1.0f) },
            { 0.35f, glm::vec4(3.0f, 1.6f, 0.5f, 0.8f) },
            { 1.00f, glm::vec4(0.8f, 0.2f, 0.04f, 0.0f) },
        });

        e.shape = EmitterShape::Sphere;
        e.shapeRadius = 0.15f;

        e.maxParticles = 100;
        e.turbulenceStrength = 0.0f;
        e.simulationSpace = SimulationSpace::World;
        e.additiveBlend = true;
        e.noiseAmount = 0.8f;
        e.spinSpeed = 2.0f;
        return e;
    }

    // Layer 2 — expanding fireball (~1.2 s): emissive gas, so additive. Fast burst that brakes hard, then boils (turbulence) while cooling along a blackbody ramp.
    inline ParticleEmitterComponent ExplosionFireball()
    {
        ParticleEmitterComponent e;
        e.looping = false;
        e.duration = 0.12f;
        e.emissionRate = 350.0f;   // ~42 particles
        e.emissionVariance = 70.0f;

        e.startLifetime = 0.9f;
        e.lifetimeVariance = 0.35f;

        e.startSpeed = 5.0f;
        e.speedVariance = 3.0f;
        e.drag = 2.5f;

        e.startSize = 0.5f;
        e.sizeVariance = 0.2f;
        e.endSize = 1.3f;          // gas expands as it thins out

        e.gravityModifier = 0.0f;

        SetColorRamp(e, {
            { 0.00f, glm::vec4(5.0f, 3.6f, 1.6f, 0.55f) },
            { 0.12f, glm::vec4(3.5f, 1.6f, 0.4f, 0.55f) },
            { 0.35f, glm::vec4(1.8f, 0.55f, 0.08f, 0.5f) },
            { 0.70f, glm::vec4(0.45f, 0.07f, 0.01f, 0.28f) },
            { 1.00f, glm::vec4(0.06f, 0.01f, 0.0f, 0.0f) },
        });

        e.shape = EmitterShape::Sphere;
        e.shapeRadius = 0.3f;

        e.maxParticles = 120;
        e.turbulenceStrength = 2.0f;
        e.simulationSpace = SimulationSpace::World;
        e.additiveBlend = true;
        e.noiseAmount = 1.0f;      // ragged, boiling edges
        e.spinSpeed = 1.5f;
        e.slowMotionDuration = 0.22f;
        e.slowMotionScale = 0.4f;
        return e;
    }

    // Layer 3 — soot cloud (~3 s): dark, alpha-blended so it occludes what is behind the fire and gives the additive layers contrast. Starts lit orange by the fireball, then chars to black. Drawn before the additive layers.
    inline ParticleEmitterComponent ExplosionSmoke()
    {
        ParticleEmitterComponent e;
        e.looping = false;
        e.duration = 0.25f;
        e.emissionRate = 96.0f;    // ~24 particles
        e.emissionVariance = 0.0f;

        e.startLifetime = 2.2f;
        e.lifetimeVariance = 0.8f;

        e.startSpeed = 2.5f;
        e.speedVariance = 1.5f;
        e.drag = 1.5f;

        e.startSize = 0.6f;
        e.sizeVariance = 0.2f;
        e.endSize = 2.2f;

        e.gravityModifier = 0.0f;

        SetColorRamp(e, {
            { 0.00f, glm::vec4(1.2f, 0.5f, 0.15f, 0.35f) },
            { 0.18f, glm::vec4(0.25f, 0.13f, 0.08f, 0.5f) },
            { 0.50f, glm::vec4(0.09f, 0.08f, 0.07f, 0.38f) },
            { 1.00f, glm::vec4(0.04f, 0.04f, 0.04f, 0.0f) },
        });

        e.shape = EmitterShape::Sphere;
        e.shapeRadius = 0.4f;

        e.maxParticles = 60;
        e.turbulenceStrength = 0.6f;
        e.simulationSpace = SimulationSpace::World;
        e.additiveBlend = false;
        e.noiseAmount = 1.2f;
        e.spinSpeed = 0.6f;
        e.slowMotionDuration = 0.22f;
        e.slowMotionScale = 0.4f;
        return e;
    }

    // Layer 4 — sparks (~1 s): many tiny incandescent streaks thrown fast in every direction, slowed by drag so they hang for a moment while cooling white -> orange -> dark.
    inline ParticleEmitterComponent ExplosionSparks()
    {
        ParticleEmitterComponent e;
        e.looping = false;
        e.duration = 0.06f;
        e.emissionRate = 2000.0f;  // ~120 particles
        e.emissionVariance = 0.0f;

        e.startLifetime = 0.9f;
        e.lifetimeVariance = 0.5f;

        e.startSpeed = 10.0f;
        e.speedVariance = 6.0f;
        e.drag = 1.5f;

        e.startSize = 0.035f;
        e.sizeVariance = 0.015f;
        e.endSize = 0.012f;

        e.gravityModifier = 0.0f;

        SetColorRamp(e, {
            { 0.00f, glm::vec4(6.0f, 4.0f, 1.5f, 1.0f) },
            { 0.30f, glm::vec4(3.0f, 1.2f, 0.25f, 1.0f) },
            { 0.70f, glm::vec4(1.0f, 0.2f, 0.03f, 0.6f) },
            { 1.00f, glm::vec4(0.2f, 0.03f, 0.0f, 0.0f) },
        });

        e.shape = EmitterShape::Sphere;
        e.shapeRadius = 0.1f;

        e.maxParticles = 200;
        e.turbulenceStrength = 0.0f;
        e.simulationSpace = SimulationSpace::World;
        e.additiveBlend = true;
        e.stretch = 0.075f;        // streaks along the direction of travel, shrinking as drag slows them
        e.slowMotionDuration = 0.22f;
        e.slowMotionScale = 0.4f;
        return e;
    }

    // Layer 5 — fine debris (~2.5 s): a few glowing flecks that cool to grey. Almost no drag: with nothing to slow them they keep flying. (The larger hull fragments are real meshes, see Game/Source/game/Explosion.hpp.)
    inline ParticleEmitterComponent ExplosionDebris()
    {
        ParticleEmitterComponent e;
        e.looping = false;
        e.duration = 0.05f;
        e.emissionRate = 400.0f;   // ~20 particles
        e.emissionVariance = 0.0f;

        e.startLifetime = 2.0f;
        e.lifetimeVariance = 0.8f;

        e.startSpeed = 6.0f;
        e.speedVariance = 3.5f;
        e.drag = 0.35f;

        e.startSize = 0.16f;
        e.sizeVariance = 0.07f;
        e.endSize = 0.1f;

        e.gravityModifier = 0.0f;

        SetColorRamp(e, {
            { 0.00f, glm::vec4(6.0f, 3.0f, 1.0f, 1.0f) },
            { 0.25f, glm::vec4(1.5f, 0.5f, 0.1f, 1.0f) },
            { 0.60f, glm::vec4(0.25f, 0.22f, 0.2f, 0.9f) },
            { 1.00f, glm::vec4(0.15f, 0.14f, 0.13f, 0.0f) },
        });

        e.shape = EmitterShape::Sphere;
        e.shapeRadius = 0.2f;

        e.maxParticles = 40;
        e.turbulenceStrength = 0.1f;
        e.simulationSpace = SimulationSpace::World;
        e.additiveBlend = false;
        e.noiseAmount = 0.5f;      // irregular chunks
        e.spinSpeed = 4.0f;        // tumbling
        e.slowMotionDuration = 0.22f;
        e.slowMotionScale = 0.4f;
        return e;
    }

    // Shockwave streaks (~0.5 s): thin shell of faint particles launched at almost the same speed. Not part of MakeExplosion() any more: at this size the shell reads as a ring of bubbles; the refraction ring (ExplosionDistortion) sells the blast front better. Kept as a building block.
    inline ParticleEmitterComponent ExplosionShockwave()
    {
        ParticleEmitterComponent e;
        e.looping = false;
        e.duration = 0.03f;
        e.emissionRate = 3000.0f;  // ~90 particles
        e.emissionVariance = 0.0f;

        e.startLifetime = 0.45f;
        e.lifetimeVariance = 0.05f;

        e.startSpeed = 16.0f;
        e.speedVariance = 1.0f;    // tight — keeps the shell thin
        e.drag = 3.0f;

        e.startSize = 0.35f;
        e.endSize = 0.05f;

        e.gravityModifier = 0.0f;

        SetColorRamp(e, {
            { 0.00f, glm::vec4(2.0f, 2.4f, 3.0f, 0.35f) },
            { 1.00f, glm::vec4(0.4f, 0.6f, 1.2f, 0.0f) },
        });

        e.shape = EmitterShape::Sphere;
        e.shapeRadius = 0.05f;

        e.maxParticles = 120;
        e.turbulenceStrength = 0.0f;
        e.simulationSpace = SimulationSpace::World;
        e.additiveBlend = true;
        e.stretch = 0.02f;         // radial streaks sharpen the blast front
        return e;
    }

    // Layer 6 — secondary blasts (~1 s): after the main detonation, fuel and ammo keep going off across the wreck. One delayed emitter that spawns puffs over a wider region, which reads as a cascade of smaller explosions scattered in time and space instead of a single perfect sphere.
    inline ParticleEmitterComponent ExplosionSecondary()
    {
        ParticleEmitterComponent e;
        e.looping = false;
        e.startDelay = 0.12f;
        e.duration = 0.45f;
        e.emissionRate = 70.0f;    // ~30 puffs
        e.emissionVariance = 25.0f;

        e.startLifetime = 0.6f;
        e.lifetimeVariance = 0.25f;

        e.startSpeed = 1.5f;
        e.speedVariance = 1.0f;
        e.drag = 1.5f;

        e.startSize = 0.55f;
        e.sizeVariance = 0.2f;
        e.endSize = 1.1f;

        e.gravityModifier = 0.0f;

        SetColorRamp(e, {
            { 0.00f, glm::vec4(5.0f, 3.5f, 1.5f, 0.7f) },
            { 0.30f, glm::vec4(2.4f, 1.0f, 0.2f, 0.65f) },
            { 0.70f, glm::vec4(0.6f, 0.1f, 0.02f, 0.35f) },
            { 1.00f, glm::vec4(0.08f, 0.02f, 0.0f, 0.0f) },
        });

        e.shape = EmitterShape::Sphere;
        e.shapeRadius = 0.6f;

        e.maxParticles = 50;
        e.turbulenceStrength = 1.0f;
        e.simulationSpace = SimulationSpace::World;
        e.additiveBlend = true;
        e.noiseAmount = 0.9f;
        e.spinSpeed = 2.0f;
        return e;
    }

    // Layer 7 — shockwave refraction (~0.7 s): one expanding ring that bends the scene behind it (screen-space distortion, no colour of its own). The alpha ramp is the strength envelope.
    inline ParticleEmitterComponent ExplosionDistortion()
    {
        ParticleEmitterComponent e;
        e.looping = false;
        e.duration = 0.01f;
        e.emissionRate = 0.0f;
        e.burstCount = 1;

        e.startLifetime = 0.8f;
        e.startSpeed = 0.0f;

        e.startSize = 0.4f;
        e.endSize = 5.0f;          // diameter; the ring rides the sprite edge

        SetColorRamp(e, {
            { 0.00f, glm::vec4(1.0f, 1.0f, 1.0f, 1.0f) },
            { 0.60f, glm::vec4(1.0f, 1.0f, 1.0f, 0.5f) },
            { 1.00f, glm::vec4(1.0f, 1.0f, 1.0f, 0.0f) },
        });

        e.shape = EmitterShape::Point;
        e.maxParticles = 2;
        e.simulationSpace = SimulationSpace::World;
        e.distortion = true;
        e.distortionStrength = 0.085f;
        return e;
    }

    // Explosion body from a flipbook: ONE sprite that plays a full pre-rendered explosion (fireball -> smoke) over its life. Sized for a 10x10 sheet whose fireball fills ~40% of the frame; replaces the procedural fireball and soot layers when a sheet is supplied (see ExplosionOptions::body). Blend: alpha, so the dark smoke in the sheet occludes; emissiveScale feeds the bright fire into the bloom.
    inline ParticleEmitterComponent ExplosionFlipbookBody()
    {
        ParticleEmitterComponent e;
        e.looping = false;
        e.duration = 0.01f;
        e.emissionRate = 0.0f;
        e.burstCount = 1;

        e.startLifetime = 2.6f;
        e.startSpeed = 0.0f;

        e.startSize = 5.5f;
        e.endSize = 5.5f;

        e.startColor = glm::vec4(1.0f);
        e.endColor = glm::vec4(1.0f);
        e.emissiveScale = 1.4f;

        e.randomRotation = false;                        // the sheet has its own up direction and rising plume
        e.fadeOutFraction = 0.35f;                       // pre-rendered smoke is still opaque on the last frame: ease it out instead of popping

        e.shape = EmitterShape::Point;
        e.maxParticles = 2;
        e.simulationSpace = SimulationSpace::World;
        e.additiveBlend = false;
        return e;
    }

    // Everything MakeExplosion() can vary.
    struct ExplosionOptions
    {
        float            scale = 2.0f;   // must match the uniform scale of the Transform the emitters are attached to
        FlipbookSettings body;           // sprite sheet with a whole explosion; empty = procedural fireball + soot
        glm::vec3        viewRight = glm::vec3(1.0f, 0.0f, 0.0f);   // camera axes: the body sprite faces the camera, so its centre offset is applied along them
        glm::vec3        viewUp = glm::vec3(0.0f, 1.0f, 0.0f);
    };

    // Convenience wrapper: returns all explosion layers as a vector, ready to attach to entities at the same world position.
    // `scale` matters because the emitter scales sizes and spawn radii, this scales speeds so the reach grows with it.
    inline std::vector<ParticleEmitterComponent> MakeExplosion(const ExplosionOptions& options)
    {
        std::vector<ParticleEmitterComponent> layers;
        layers.push_back(ExplosionFlash());

        if (!options.body.texture.empty())
        {
            ParticleEmitterComponent body = ExplosionFlipbookBody();
            ApplyFlipbook(body, options.body);
            // Cancel the sheet's off-centre origin so the animation starts on the emitter, whatever the camera orientation.
            body.spawnOffset = -(options.viewRight * options.body.centerOffset.x + options.viewUp * options.body.centerOffset.y) * body.startSize;
            layers.push_back(body);
        }
        else
        {
            layers.push_back(ExplosionFireball());
            layers.push_back(ExplosionSmoke());
        }

        layers.push_back(ExplosionSparks());
        layers.push_back(ExplosionDebris());
        layers.push_back(ExplosionSecondary());
        layers.push_back(ExplosionDistortion());

        const float speedFactor = options.scale / 2.0f;   // layers are tuned for scale 2
        for (auto& layer : layers)
        {
            layer.startSpeed *= speedFactor;
            layer.speedVariance *= speedFactor;
        }
        return layers;
    }

    inline std::vector<ParticleEmitterComponent> MakeExplosion(float scale = 2.0f)
    {
        ExplosionOptions options;
        options.scale = scale;
        return MakeExplosion(options);
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
