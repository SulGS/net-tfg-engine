#version 430 core

// Emissive beam for the arena's laser walls/spokes (pairs with laser_wall.vert), drawn additively (SRC_ALPHA, ONE): never
// lit or opaque, the tube is just a canvas and HDR output drives bloom. Look: profile fading to 0 at the silhouette, gaussian
// halo, wandering white filament, shimmer. uIntensity drives brightness AND width; uWarning/uWarnTension = amber stutter preview.

in vec3  vWorldPos;
in vec3  vAxisW;
in vec3  vRadialW;
in float vAlong;
in float vEnd;

layout(location = 0) out vec4 FragColor;

// The additive pass only sets this one (plus uModel/uView/uProjection) — no
// texture units are bound, see RenderSystem::AdditivePass.
uniform vec3 uCameraPos;

// Animated per frame by LaserWallRenderSystem
uniform float uTime;
uniform float uIntensity;    // 0 = off .. 1 = fully energised
uniform float uWarning;      // 0..1 blend towards the warning look
uniform float uWarnTension;  // 0..1 over the warning window: stutter goes from mostly-off to mostly-on
uniform float uFlash;        // 1 the instant the beam energises, decays to 0

// Set once per Material
uniform float uSeed;         // desyncs walls from each other
uniform vec3  uBeamColor;    // halo, normal state
uniform vec3  uWarnColor;    // halo, warning state
uniform vec3  uHotColor;     // filament, HDR
uniform float uGlowStrength;
uniform float uCoreStrength;

// -------------------------------------------------------
// Noise (same helpers as lava.frag, plus a 1D variant)
// -------------------------------------------------------
float hash11(float p)
{
    p = fract(p * 0.1031);
    p *= p + 33.33;
    p *= p + p;
    return fract(p);
}

float noise1(float x)
{
    float i = floor(x);
    float f = fract(x);
    f = f * f * (3.0 - 2.0 * f);
    return mix(hash11(i), hash11(i + 1.0), f);
}

float hash21(vec2 p)
{
    p = fract(p * vec2(123.34, 456.21));
    p += dot(p, p + 45.32);
    return fract(p.x * p.y);
}

float valueNoise(vec2 p)
{
    vec2 i = floor(p);
    vec2 f = fract(p);
    float a = hash21(i);
    float b = hash21(i + vec2(1.0, 0.0));
    float c = hash21(i + vec2(0.0, 1.0));
    float d = hash21(i + vec2(1.0, 1.0));
    vec2  u = f * f * (3.0 - 2.0 * f);
    return mix(mix(a, b, u.x), mix(c, d, u.x), u.y);
}

// 3 octaves, normalised to roughly 0..1
float fbm(vec2 p)
{
    float sum = 0.0;
    float amp = 0.5;
    for (int i = 0; i < 3; i++)
    {
        sum += amp * valueNoise(p);
        p   *= 2.03;
        amp *= 0.5;
    }
    return sum / 0.875;
}

void main()
{
    // ---- Cross-section coordinate ---- Mesh normals tilt at the rings, so rebuild the profile: s = -1..1 across
    // the tube as the camera sees it (0 = line facing the camera, +-1 = silhouette).
    vec3  V       = normalize(uCameraPos - vWorldPos);
    vec3  A       = normalize(vAxisW);
    vec3  R       = normalize(vRadialW);
    vec3  side    = cross(A, V);
    float sideLen = length(side);
    float s       = (sideLen > 1e-3) ? dot(R, side / sideLen) : 0.0;

    // ---- Power: width + brightness -----------------------------------------
    float power = clamp(uIntensity, 0.0, 1.0);
    float width = mix(0.06, 1.0, power * power * (3.0 - 2.0 * power));
    float sn    = s / width;              // -1..1 across the *visible* beam
    if (abs(sn) > 1.0) discard;           // contributes exactly 0 out there anyway: skip the work

    float d  = abs(sn);
    float d2 = d * d;

    // ---- Profile ------------------------------------------------------------
    float t = uTime;

    // Filament wanders inside the tube; low-frequency so it drifts, not jitters.
    float wander = (fbm(vec2(vAlong * 0.10 + uSeed, t * 1.6)) - 0.5) * 0.7;
    float dc     = abs(sn - wander);

    // (1 - d2) forces every term to 0 at the silhouette: additive blending then
    // leaves nothing of the model's edge visible.
    float edge = 1.0 - d2;
    float core = exp(-(dc / 0.20) * (dc / 0.20)) * edge;   // filament
    float hot  = exp(-(dc / 0.08) * (dc / 0.08)) * edge;   // white-hot centre of it
    float halo = exp(-2.5 * d2) * edge;

    // ---- Energy animation ---------------------------------------------------
    // Ripples racing along the beam. The direction differs per wall (it follows
    // the mesh's local Z), which keeps neighbours from looking mirrored.
    float flow    = fbm(vec2(vAlong * 0.30 - t * 3.2 + uSeed * 7.0, sn * 1.4 + t * 0.4));
    float shimmer = mix(0.65, 1.35, smoothstep(0.25, 0.75, flow));

    // Slow bands, a touch of "scanning" so the beam isn't uniformly bright.
    float band = 0.88 + 0.12 * sin(vAlong * 0.55 - t * 5.5 + uSeed * 3.0);

    // Rare, short dips in the supply.
    float mains = 1.0 - 0.28 * smoothstep(0.82, 1.0, noise1(t * 13.0 + uSeed * 31.0));

    // Slightly hotter at the ends, where the beam leaves its emitter.
    float ends = smoothstep(0.80, 1.0, abs(vEnd));

    // ---- Warning look ---- Irregular stutter instead of a square blink. The on-threshold falls as the window
    // runs out, so it starts mostly dark and ends mostly lit — the beam "wants" to fire.
    float stutterOn = smoothstep(0.0, 0.12,
        noise1(t * 11.0 + uSeed * 17.0) - mix(0.75, 0.25, uWarnTension));
    float stutter   = mix(1.0, 0.2 + 0.8 * stutterOn, uWarning);

    vec3 beamCol = mix(uBeamColor, uWarnColor, uWarning);

    // ---- Compose ------------------------------------------------------------
    vec3 col = beamCol * halo * shimmer * uGlowStrength
             + uHotColor * (core * 0.7 + hot * 0.9 + ends * core * 0.6) * uCoreStrength;

    // Energise flash: a white-hot burst that rides on top and decays.
    col += uHotColor * uFlash * (core + 0.5 * halo) * 1.8;

    col *= band * mains * stutter * (0.25 + 0.75 * power);

    // Alpha is the additive weight (SRC_ALPHA, ONE): 1.0 adds col as-is.
    FragColor = vec4(col, 1.0);
}
