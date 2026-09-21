#version 430 core

// The shot's charge-up: energy gathering into an orb at the ship's muzzle. Pairs
// with glow_volume.vert; drawn additively by RenderSystem::AdditivePass
// (MeshComponent::additive), so the output is light added on top of the scene.
//
// The canvas is a unit sphere (uniformly scaled here). The glow is raymarched
// inside it — a few samples of an analytic density accumulated along the view
// ray — so it reads as a ball of plasma from any angle:
//   - a soft halo that fills the sphere
//   - a white-hot core that tightens and brightens as the charge builds
//   - a thin shell of sparks that CONTRACTS towards the centre, as if energy were
//     being pulled in

in vec3 vLocalPos;
in vec3 vCamLocal;

layout(location = 0) out vec4 FragColor;

// Animated per frame by ChargingBulletRenderSystem
uniform float uTime;
uniform float uProgress;  // 0..1 across the charge (the last frame is 1)

// Set once per Material
uniform float uSeed;
uniform vec3  uColor;      // halo and sparks
uniform vec3  uHotColor;   // core, HDR
uniform float uGlowStrength;
uniform float uCoreStrength;

const int STEPS = 12;

float hash13(vec3 p)
{
    p = fract(p * 0.1031);
    p += dot(p, p.zyx + 31.32);
    return fract((p.x + p.y) * p.z);
}

float noise3(vec3 p)
{
    vec3 i = floor(p);
    vec3 f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    float n000 = hash13(i);
    float n100 = hash13(i + vec3(1, 0, 0));
    float n010 = hash13(i + vec3(0, 1, 0));
    float n110 = hash13(i + vec3(1, 1, 0));
    float n001 = hash13(i + vec3(0, 0, 1));
    float n101 = hash13(i + vec3(1, 0, 1));
    float n011 = hash13(i + vec3(0, 1, 1));
    float n111 = hash13(i + vec3(1, 1, 1));
    return mix(mix(mix(n000, n100, f.x), mix(n010, n110, f.x), f.y),
               mix(mix(n001, n101, f.x), mix(n011, n111, f.x), f.y), f.z);
}

void main()
{
    vec3  o    = vCamLocal;
    vec3  d    = normalize(vLocalPos - vCamLocal);
    float b    = dot(o, d);
    float disc = b * b - (dot(o, o) - 1.0);
    if (disc <= 0.0) discard;

    float sq = sqrt(disc);
    float t0 = max(-b - sq, 0.0);
    float t1 = -b + sq;
    float dt = (t1 - t0) / float(STEPS);

    float prog = clamp(uProgress, 0.0, 1.0);

    // The shell falls from near the edge of the sphere to near the core.
    float shellR = mix(0.90, 0.22, prog);

    vec3 acc = vec3(0.0); // x = halo, y = core, z = sparks
    for (int i = 0; i < STEPS; ++i)
    {
        vec3  p = o + d * (t0 + (float(i) + 0.5) * dt);
        float r = length(p);

        float halo = exp(-r * r / 0.10);
        float core = exp(-r * r / (0.006 + 0.03 * prog));

        // Sparks: fine, streaky bits of a thin shell, sliding inwards over time
        // (the noise is sampled at a radius that shrinks with time, so the
        // pattern appears to be sucked towards the centre).
        vec3  dir    = p / max(r, 1e-3);
        float sparks = smoothstep(0.55, 0.85,
            noise3(dir * vec3(9.0, 9.0, 4.0) + vec3(uSeed, uTime * 6.0, -uTime * 4.0)));
        float shell  = exp(-pow((r - shellR) / 0.035, 2.0)) * (0.1 + 2.2 * sparks);

        acc += vec3(halo, core, shell) * dt;
    }

    // Flicker faster as the charge builds.
    float flicker = 0.88 + 0.12 * sin(uTime * (30.0 + 40.0 * prog) + uSeed * 7.0);

    float build = prog * prog;
    vec3 col = uColor * (acc.x * (0.10 + 0.90 * build) + acc.z * (0.7 + 0.6 * prog)) * uGlowStrength
             + uHotColor * acc.y * (0.15 + 1.85 * build) * uCoreStrength;
    col *= flicker;

    FragColor = vec4(col, 1.0);
}
