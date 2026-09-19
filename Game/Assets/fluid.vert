#version 430 core

// Shares the exact same attribute/uniform/varying interface as ggx.vert (see
// that file) so fluid materials plug into the same Forward+ pipeline and
// ShadingPass() loop with zero engine-side changes. The only addition is a
// Gerstner-wave displacement of aPos before the standard world-space /
// TBN computation, so ggx.frag-style lighting still gets a correct normal
// for the displaced surface.
//
// Assumes the mesh is authored as a flat plane with its rest normal along
// local +Z (the usual convention for a water/lava plane) — displacement is
// applied in local space, then carried into world space by uModel like any
// other vertex attribute, so entity Transform (position/rotation/scale)
// still places/orients it exactly as with any other mesh.

layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aUV;
layout(location = 3) in vec4 aTangent; // xyz = tangent, w = bitangent sign

uniform mat4 uModel;
uniform mat4 uView;
uniform mat4 uProjection;

uniform float uTime;
// 0 = perfectly flat (e.g. lava barely swelling); bigger = choppier water.
uniform float uWaveAmplitude;
uniform float uWaveSpeed;
// Bigger uWaveScale = larger, slower-looking waves (it divides the wave
// frequency), smaller = tighter chop.
uniform float uWaveScale;

out vec3 vWorldPos;
out vec2 vUV;
out vec3 vT;
out vec3 vB;
out vec3 vN;

// Sum of 4 Gerstner waves. Returns the local-space offset and, via dPdx/dPdy,
// the surface's partial derivatives (needed to build the analytic normal —
// far cheaper and more stable than finite-differencing neighbour vertices).
vec3 GerstnerDisplace(vec2 posXY, out vec3 dPdx, out vec3 dPdy)
{
    const vec2  dirs[4]   = vec2[](vec2(1.0, 0.0), vec2(0.6, 0.8), vec2(-0.7, 0.5), vec2(-0.3, -0.9));
    const float freqs[4]  = float[](1.0, 1.7, 2.3, 3.1);
    const float amps[4]   = float[](1.0, 0.55, 0.35, 0.2);
    const float speeds[4] = float[](1.0, 1.3, 0.8, 1.6);

    vec3 offset = vec3(0.0);
    dPdx = vec3(1.0, 0.0, 0.0);
    dPdy = vec3(0.0, 1.0, 0.0);

    float invScale = 1.0 / max(uWaveScale, 0.0001);

    for (int i = 0; i < 4; i++)
    {
        vec2  d     = normalize(dirs[i]);
        float k     = freqs[i] * invScale;
        float w     = d.x * posXY.x + d.y * posXY.y;
        float phase = k * w + speeds[i] * uWaveSpeed * uTime;
        float a     = amps[i] * uWaveAmplitude;

        float s = sin(phase);
        float c = cos(phase);

        // Steepness (Q) factor: caps how far this wave pulls points toward
        // its crest so that, even with all 4 waves' crests aligned, the
        // combined horizontal pull can't exceed kMaxSteepness and fold the
        // surface over itself. Without this, offset.x/y below grows with
        // uWaveAmplitude unbounded while the underlying point spacing does
        // not, so steep/choppy settings can self-intersect.
        const float kMaxSteepness = 0.9;
        float q = min(1.0, (kMaxSteepness / 4.0) / max(a * k, 1e-4));

        // Vertical bob plus a small horizontal pinch toward the wave crest —
        // the pinch is what gives Gerstner waves their sharper, choppier
        // crest shape instead of a plain sine's smooth round top.
        offset.z += a * s;
        offset.x += q * d.x * a * c;
        offset.y += q * d.y * a * c;

        float dS = a * k * c;
        dPdx += vec3(-q * d.x * d.x * a * k * s, -q * d.x * d.y * a * k * s, d.x * dS);
        dPdy += vec3(-q * d.x * d.y * a * k * s, -q * d.y * d.y * a * k * s, d.y * dS);
    }

    return offset;
}

void main()
{
    vec3 dPdx, dPdy;
    vec3 displaced = aPos + GerstnerDisplace(aPos.xy, dPdx, dPdy);

    vec4 worldPos = uModel * vec4(displaced, 1.0);
    vWorldPos     = worldPos.xyz;
    vUV           = aUV;

    // Same scale-stripping trick as ggx.vert — see that file for why.
    mat3 modelMat   = mat3(uModel);
    vec3 modelScale = vec3(length(modelMat[0]), length(modelMat[1]), length(modelMat[2]));
    mat3 rotOnly    = mat3(
        modelMat[0] / modelScale.x,
        modelMat[1] / modelScale.y,
        modelMat[2] / modelScale.z
    );

    // Unlike aNormal on a static mesh, the wave normal is generated here from
    // local-space slopes, so it still needs the model's scale undone per
    // axis, not just its rotation: a non-uniformly scaled plane (e.g. a wide
    // (50,50,1) fluid surface) stretches the waves wider in world space
    // without changing their height, so shading the stretched surface with
    // rotOnly alone would use the pre-stretch (steeper) local slopes. This
    // divides rotOnly by the same per-axis scale again, which is equivalent
    // to R * S^-1 (the correct normal transform for an R*S model matrix with
    // no shear) without calling mat3 inverse() and its precision cost at
    // large uniform scale. aTangent doesn't need this: it's axis-aligned
    // (local +X) on this quad mesh, so scale doesn't change its direction
    // once normalized.
    mat3 normalMat = mat3(
        rotOnly[0] / modelScale.x,
        rotOnly[1] / modelScale.y,
        rotOnly[2] / modelScale.z
    );

    // Analytic normal of the displaced surface replaces aNormal; falls back
    // to it gracefully since dPdx/dPdy start at the flat basis (1,0,0)/(0,1,0)
    // when uWaveAmplitude is 0.
    vec3 waveNormal = normalize(cross(dPdx, dPdy));

    vec3 N = normalize(normalMat * waveNormal);
    vec3 T = normalize(rotOnly * aTangent.xyz);
    T      = normalize(T - dot(T, N) * N);
    vec3 B = cross(N, T) * aTangent.w;

    vT = T;
    vB = B;
    vN = N;

    gl_Position = uProjection * uView * worldPos;
}
