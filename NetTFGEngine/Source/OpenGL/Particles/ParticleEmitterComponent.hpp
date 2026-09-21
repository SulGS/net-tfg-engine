#ifndef PARTICLE_EMITTER_COMPONENT_HPP
#define PARTICLE_EMITTER_COMPONENT_HPP

#include <glm/glm.hpp>
#include <functional>
#include <string>
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
    float     seed = 0.0f; // random [0,1) fixed at spawn: drives sprite rotation and noise pattern
    float     spin = 0.0f; // rotation speed in rad/s
    bool      alive = false;
};

// GPU-side layout — matches the SSBO binding in the particle billboard shader (std430).
struct GPUParticle {
    glm::vec4 positionSize;  // xyz = world pos, w = size
    glm::vec4 color;         // rgba — lerped from colorStart→colorEnd
    glm::vec4 velocitySeed;  // xyz = world velocity, w = seed
    glm::vec4 params;        // x = stretch, y = noiseAmount (distortion batches: strength), z = rotation (rad), w = normalised age [0,1]
    glm::vec4 flip;          // flipbook: x = columns, y = rows, z = frame (fractional -> blended with the next), w = frame count
};

// One stop of a colour-over-lifetime ramp. rgb is linear and may exceed 1 (HDR) so bloom picks it up; a is straight alpha.
struct ColorStop {
    float     t = 0.0f;                    // normalised particle age [0,1]
    glm::vec4 color = glm::vec4(1.0f);
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

enum class FlipbookPlayback {
    OverLifetime,  // frame advances from 0 to last across the particle's lifetime (explosions, puffs)
    FixedFps,      // frame = age * flipbookFps, looping
    RandomFrame,   // one random frame per particle, held for its whole life
};

// How the sprite sheet contributes to the final pixel.
enum class FlipbookAlpha {
    TextureAlpha,  // rgb = texture.rgb * tint, a = texture.a * tint.a        (sheets with a real alpha channel)
    Luminance,     // rgb = tint,               a = texture luminance * tint.a (greyscale masks, alpha blend: smoke)
    Additive,      // rgb = texture.rgb * tint, a = tint.a                     (black-background sheets, additive: fire)
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
    float sizeVariance = 0.0f;   // +/- jitter on startSize/endSize per particle (world units, before emitter scale)
    float drag = 0.0f;           // exponential velocity damping (1/s): v *= exp(-drag*dt). 0 = none
    float startDelay = 0.0f;     // seconds to wait before emission begins (non-looping: `duration` counts from here)

    // --- Sprite look: 0 on all three = plain soft round disc ---
    float stretch = 0.0f;        // streak length in seconds of travel: quad is elongated along on-screen velocity by stretch * speed (sparks)
    float noiseAmount = 0.0f;    // 0..~1.5: irregular, shredded edges that erode further as the particle ages (fire, smoke)
    float spinSpeed = 0.0f;      // max |rotation speed| in rad/s, random sign/magnitude per particle
    bool  randomRotation = true; // start every sprite at a random angle; false keeps sprites upright (flipbooks with baked-in lighting/rise)
    glm::vec3 spawnOffset = glm::vec3(0.0f); // local offset added to every spawn position (scaled by the emitter scale)

    // --- Colour ramp (optional): overrides startColor -> endColor with up to kMaxColorStops stops sorted by t. rgb may be > 1 (HDR). colorRampCount == 0 keeps the plain two-colour lerp ---
    static constexpr int kMaxColorStops = 6;
    ColorStop colorRamp[kMaxColorStops];
    int       colorRampCount = 0;
    float     emissiveScale = 1.0f;   // multiplies rgb of startColor / endColor / ramp: HDR intensity without editing every colour

    // --- Flipbook (optional): sprite-sheet animation. texture = asset name as packed in the scene's .ntfg; empty keeps the procedural disc.
    //     Frames are read left-to-right, top-to-bottom (frame 0 = top-left). ---
    std::string      texture;
    int              flipbookCols = 1;
    int              flipbookRows = 1;
    int              flipbookFrames = 0;      // 0 = cols * rows
    FlipbookPlayback flipbookPlayback = FlipbookPlayback::OverLifetime;
    float            flipbookFps = 24.0f;     // FixedFps only
    bool             flipbookBlend = true;    // cross-fade between adjacent frames
    FlipbookAlpha    flipbookAlpha = FlipbookAlpha::TextureAlpha;

    // --- Screen-space distortion: instead of drawing colour, the sprite refracts what is behind it (shockwave, heat haze). Sprite is a radial ring; alpha of the colour ramp is the strength envelope ---
    bool  distortion = false;
    float distortionStrength = 0.04f;  // max displacement as a fraction of the screen height

    // --- Burst: spawn this many particles at once when emission starts (on top of emissionRate) ---
    int burstCount = 0;

    // --- Impact slow motion: particles of this emitter age at slowMotionScale x speed at the start, easing back to normal over slowMotionDuration seconds ---
    float slowMotionDuration = 0.0f;
    float slowMotionScale = 0.3f;

    // --- Fade out: over the last `fadeOutFraction` of every particle's life its alpha eases to 0, so it never pops out while still visible (0 = off) ---
    float fadeOutFraction = 0.0f;

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
    bool      burstFired = false;   // burstCount already spawned
    bool      done = false;    // true once a non-looping emitter has finished and all particles died; poll to know when to remove/recycle
    glm::vec3 emitterLastPos = glm::vec3(0.0f); // previous world position, used by SimulationSpace::Local
};

#endif // PARTICLE_EMITTER_COMPONENT_HPP