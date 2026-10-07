#version 430 core

// Laser shield: an energy bubble around a ship. Pairs with glow_volume.vert, drawn additively (AdditivePass), same canvas
// as the bolt/charge glows (charge.glb, a UNIT SPHERE scaled to the shield's radius). The shell is a sphere of radius
// SHELL_R inside it, evaluated analytically where the view ray crosses it (front and back face) instead of raymarched:
// it is thin, and a 12-step march would band. On the shell: a globe of drifting meridians/parallels (the HUD icon's
// motif) carried by an energy flow, brighter at grazing angles (fresnel), plus a soft halo just outside the silhouette.
// Like the walls: a stutter while it is about to run out; and it spins up from the equator and shatters when broken.

in vec3 vLocalPos;
in vec3 vCamLocal;

layout(location = 0) out vec4 FragColor;

// Animated per frame by ShieldRenderSystem
uniform float uTime;
uniform float uPower;    // 0..1 spin-up when raised (the shell grows from the equator to the poles)
uniform float uExpire;   // 0 = plenty of time left .. 1 = about to run out (stutter, like a wall about to energise)
uniform float uBreak;    // 0 = intact .. 1 = fully shattered (hit by a bullet)
uniform float uFade;     // 1 = up .. 0 = gone (expired: collapses without shattering)

// Set once per Material
uniform float uSeed;         // desyncs shields from each other
uniform vec3  uColor;        // shell + halo
uniform vec3  uHotColor;     // lines, spin-up edge, break flash; HDR
uniform float uGlowStrength;
uniform float uCoreStrength;

const float SHELL_R = 0.82;
const float PI      = 3.14159265;

// -------------------------------------------------------
// Noise (same helpers as laser_charge.frag / laser_wall.frag)
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

// Light emitted by the shell at unit-sphere direction n, seen at incidence cosine cosT. x = glow, y = hot lines.
vec2 shellEmission(vec3 n, float cosT, float radius)
{
    // Fresnel-ish: bright at grazing angles, nearly clear face-on, so the ship stays visible through the bubble.
    // Clamped: cosT can round past 1 face-on, and pow with a negative base is undefined (NaN on Intel/AMD).
    float rim = 0.10 + 2.6 * pow(clamp(1.0 - cosT, 0.0, 1.0), 3.0);

    // Globe lines: 4 meridians drifting around the ship's vertical axis, 2 parallels breathing up and down.
    float lon  = atan(n.y, n.x) + uTime * 0.7 + uSeed;
    float lat  = asin(clamp(n.z, -1.0, 1.0));
    float mer  = abs(sin(lon * 2.0));
    float par  = abs(sin(lat * 2.0 + sin(uTime * 1.3 + uSeed) * 0.35));
    float lineW = 0.05 * radius / SHELL_R; // the shattering shell expands: keep the lines the same angular width
    float lines = exp(-pow(mer / lineW, 2.0)) + exp(-pow(par / lineW, 2.0));

    // Energy flow scrolling over the surface: a shimmer between dim and bright patches, like the walls'.
    float flow    = noise3(n * 3.5 + vec3(uSeed, uSeed * 0.7, -uTime * 1.8));
    float shimmer = mix(0.45, 1.4, smoothstep(0.25, 0.8, flow));

    // Spin-up: the shell exists up to |latitude| < power, with a hot seam on the growing edge.
    float reach  = uPower * 1.15;
    float reveal = 1.0 - smoothstep(reach - 0.12, reach, abs(n.z));
    float sd     = (abs(n.z) - reach) / 0.05; // negative inside the revealed band: x*x, not pow
    float seam   = exp(-sd * sd) * (1.0 - smoothstep(0.85, 1.0, uPower));

    // Shatter: the surface breaks into noise cells that drop out as the break progresses.
    float cells   = noise3(n * 5.0 + uSeed * 3.0);
    float shatter = (uBreak > 0.0) ? smoothstep(uBreak * 1.15 - 0.08, uBreak * 1.15, cells) : 1.0;

    float glow = rim * shimmer * reveal * shatter;
    float hot  = (lines * shimmer * reveal + seam * 2.0) * shatter * (0.45 + rim);
    return vec2(glow, hot);
}

void main()
{
    vec3  o = vCamLocal;
    vec3  d = normalize(vLocalPos - vCamLocal);
    float b = dot(o, d);
    float c = dot(o, o);

    // The shattered shell flies apart: its radius grows a bit while it breaks (staying inside the unit canvas).
    float radius = SHELL_R * (1.0 + 0.15 * uBreak);

    // Rays that pass well clear of the shell and its halo add nothing: skip the work.
    float closest = sqrt(max(c - b * b, 0.0));
    if (closest > radius + 0.16) discard;

    vec2 em = vec2(0.0);

    // Shell: front and back face. The back face is seen through the ship and the bubble, so it's dimmer.
    float disc = b * b - (c - radius * radius);
    if (disc > 0.0)
    {
        float sq = sqrt(disc);
        float tF = -b - sq;
        float tB = -b + sq;
        if (tF > 0.0)
        {
            vec3 n = (o + d * tF) / radius;
            em += shellEmission(n, abs(dot(n, d)), radius);
        }
        if (tB > 0.0)
        {
            vec3 n = (o + d * tB) / radius;
            em += shellEmission(n, abs(dot(n, d)), radius) * 0.4;
        }
    }

    // Halo: falls off with the ray's closest distance to the shell, outside the silhouette (inside it the shell's rim
    // term already glows). Fades with the spin-up and the break.
    float halo    = exp(-pow(max(closest - radius, 0.0) / 0.07, 2.0)) * step(radius, closest)
                  * uPower * (1.0 - uBreak);

    // Running out: irregular stutter whose off-time grows as the end nears (mirror of laser_wall.frag's warning, which
    // goes from mostly-off to mostly-on).
    float stutterOn = smoothstep(0.0, 0.12, noise1(uTime * 11.0 + uSeed * 17.0) - mix(0.15, 0.6, uExpire));
    float stutter   = (uExpire > 0.0) ? mix(1.0, 0.25 + 0.75 * stutterOn, smoothstep(0.0, 0.2, uExpire)) : 1.0;

    // Break flash: a white-hot burst over the whole bubble, gone in the first part of the shatter.
    float flash = (uBreak > 0.0) ? pow(1.0 - uBreak, 3.0) : 0.0;

    vec3 col = uColor * (em.x * 0.35 + halo * 0.8) * uGlowStrength
             + uHotColor * em.y * 0.5 * uCoreStrength;
    col *= stutter;
    col += uHotColor * flash * (em.x * 0.6 + em.y + 0.3 * halo) * 2.0;
    col *= uFade * (1.0 - smoothstep(0.6, 1.0, uBreak));

    // Alpha is the additive weight (SRC_ALPHA, ONE): 1.0 adds col as-is.
    FragColor = vec4(col, 1.0);
}
