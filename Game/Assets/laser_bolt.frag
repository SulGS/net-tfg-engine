#version 430 core

// The laser shot: a plasma bolt with a white-hot core, a coloured halo and a
// turbulent tail. Pairs with glow_volume.vert; drawn additively by
// RenderSystem::AdditivePass (MeshComponent::additive), so the output is light
// added on top of the scene.
//
// The canvas is a unit sphere stretched by the entity transform into an ellipsoid
// whose LOCAL +X is the direction of flight (the bullet is rotated about Z to face
// its velocity). In local space x runs from -1 (far end of the tail) to +1 (front
// of the canvas), and the head of the bolt sits just ahead of the middle, at
// HEAD_X — that's where the collision box is, the tail trails behind it.
//
// The glow is raymarched through the sphere: a few samples of an analytic density
// (a head blob, a tail that narrows and fades away from it, a thin white filament
// along the axis) accumulated along the view ray. That is what makes it read as a
// volume of plasma from any angle instead of a flat streak.

in vec3 vLocalPos;
in vec3 vCamLocal;

layout(location = 0) out vec4 FragColor;

// Animated per frame by BulletRenderSystem
uniform float uTime;
uniform float uAge;   // seconds since the bullet appeared (spawn flash + ramp-up)
uniform float uFade;  // 1 = alive .. 0 = about to expire (dissipates instead of vanishing)

// Set once per Material
uniform float uSeed;  // desyncs bolts from each other
uniform vec3  uColor;      // halo
uniform vec3  uHotColor;   // filament + head, HDR
uniform float uGlowStrength;
uniform float uCoreStrength;

const int   STEPS  = 12;
const float HEAD_X = 0.10;

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

// Density of the bolt at a point of the unit sphere: x = halo, y = filament/core.
vec2 boltDensity(vec3 p, float spawnFlash)
{
    float x  = p.x;
    float r2 = dot(p.yz, p.yz);

    // Head: a compact, roundish blob. It swells for an instant when the bolt is born.
    float dh   = x - HEAD_X;
    float head = exp(-(dh * dh / 0.008 + r2 / 0.14)) * (1.0 + 2.0 * spawnFlash);

    // Tail: how far along from the far end towards the head (0..1). It gets wider
    // and denser closer to the head, and is torn up by noise scrolling backwards.
    float tx   = smoothstep(-1.0, HEAD_X, x);
    float w    = mix(0.05, 0.30, tx);
    float turb = noise3(vec3(x * 3.0 + uTime * 12.0 + uSeed, p.y * 4.0, p.z * 4.0));
    float tail = exp(-r2 / (w * w)) * tx * tx * (0.45 + 1.1 * turb)
               * (1.0 - smoothstep(HEAD_X - 0.03, HEAD_X + 0.12, x));

    // Filament: the thin white-hot line down the middle of the tail, into the head.
    float axis = exp(-r2 / 0.006) * pow(tx, 1.2)
               * (1.0 - smoothstep(HEAD_X, HEAD_X + 0.15, x));

    return vec2(head + 0.9 * tail, head * 0.8 + axis * 1.4);
}

void main()
{
    // Ray through the fragment, in local (unit sphere) space.
    vec3  o    = vCamLocal;
    vec3  d    = normalize(vLocalPos - vCamLocal);
    float b    = dot(o, d);
    float disc = b * b - (dot(o, o) - 1.0);
    if (disc <= 0.0) discard;

    float sq = sqrt(disc);
    float t0 = max(-b - sq, 0.0);
    float t1 = -b + sq;
    float dt = (t1 - t0) / float(STEPS);

    float spawnFlash = exp(-uAge * 20.0);

    vec2 acc = vec2(0.0);
    for (int i = 0; i < STEPS; ++i)
    {
        vec3 p = o + d * (t0 + (float(i) + 0.5) * dt);
        acc += boltDensity(p, spawnFlash) * dt;
    }

    // Short ramp-in so the bolt swells out of the muzzle instead of popping in,
    // and a fast shimmer so it never looks like a static sprite.
    float life    = smoothstep(0.0, 0.05, uAge) * uFade;
    float shimmer = 0.9 + 0.1 * sin(uTime * 60.0 + uSeed * 5.0);

    vec3 col = uColor * acc.x * uGlowStrength + uHotColor * acc.y * uCoreStrength;
    col *= life * shimmer;

    FragColor = vec4(col, 1.0);
}
