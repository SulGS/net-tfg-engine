#version 430 core

// Shares the exact same attribute/uniform/varying interface as the engine default vertex shader (DefaultShader.hpp) (see
// that file) so fluid materials plug into the same Forward+ pipeline and
// ShadingPass() loop with zero engine-side changes. The only addition is a
// Gerstner-wave displacement of aPos before the standard world-space /
// TBN computation, so the default shader's lighting (DefaultShader.hpp) still gets a correct normal
// for the displaced surface.
//
// Assumes the mesh is authored as a flat plane lying in local XZ with its rest
// normal along local +Y — glTF's Y-up convention, which is what a plane lying on
// the ground exports as from Blender. The waves run over the plane's two axes
// (x, z) and rise along its normal (y). Displacement is applied in local space,
// then carried into world space by uModel like any other vertex attribute, so
// entity Transform (position/rotation/scale) still places/orients it exactly as
// with any other mesh.
//
// (An earlier version assumed a plane in XY with a +Z normal. On a mesh like this
// one that made the waves depend on x alone, sent most of the displacement
// sideways along the plane instead of up, and pointed the shading normal along
// the plane instead of away from it.)

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

// Sum of 4 Gerstner waves over the plane, in the basis (u, v, h): u and v are the
// two axes of the plane and h rises along its normal. Returns the offset in that
// basis and, via dPdu/dPdv, the surface's partial derivatives (needed to build the
// analytic normal — far cheaper and more stable than finite-differencing neighbour
// vertices). main() maps (u, v, h) onto the mesh's (x, z, y).
vec3 GerstnerDisplace(vec2 planeUV, out vec3 dPdu, out vec3 dPdv)
{
    const vec2  dirs[4]   = vec2[](vec2(1.0, 0.0), vec2(0.6, 0.8), vec2(-0.7, 0.5), vec2(-0.3, -0.9));
    const float freqs[4]  = float[](1.0, 1.7, 2.3, 3.1);
    const float amps[4]   = float[](1.0, 0.55, 0.35, 0.2);
    const float speeds[4] = float[](1.0, 1.3, 0.8, 1.6);

    vec3 offset = vec3(0.0);
    dPdu = vec3(1.0, 0.0, 0.0);
    dPdv = vec3(0.0, 1.0, 0.0);

    float invScale = 1.0 / max(uWaveScale, 0.0001);

    for (int i = 0; i < 4; i++)
    {
        vec2  d     = normalize(dirs[i]);
        float k     = freqs[i] * invScale;
        float w     = d.x * planeUV.x + d.y * planeUV.y;
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

        // Vertical bob (h) plus a small horizontal pinch (u, v) toward the wave
        // crest — the pinch is what gives Gerstner waves their sharper, choppier
        // crest shape instead of a plain sine's smooth round top.
        offset.z += a * s;
        offset.x += q * d.x * a * c;
        offset.y += q * d.y * a * c;

        float dS = a * k * c;
        dPdu += vec3(-q * d.x * d.x * a * k * s, -q * d.x * d.y * a * k * s, d.x * dS);
        dPdv += vec3(-q * d.x * d.y * a * k * s, -q * d.y * d.y * a * k * s, d.y * dS);
    }

    return offset;
}

void main()
{
    // Waves are computed over the plane's axes (x, z) and rise along y. The (u, v, h)
    // results are mapped onto the mesh with .xzy: u -> x, v -> z, h -> y.
    vec3 dPdu, dPdv;
    vec3 wave      = GerstnerDisplace(aPos.xz, dPdu, dPdv);
    vec3 displaced = aPos + wave.xzy;

    vec4 worldPos = uModel * vec4(displaced, 1.0);
    vWorldPos     = worldPos.xyz;
    vUV           = aUV;

    // Same scale-stripping trick as the default vertex shader (DefaultShader.hpp) — see the comment there for why.
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
    // (50,1,50) fluid surface) stretches the waves wider in world space
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

    // Analytic normal of the displaced surface replaces aNormal. The tangents are
    // taken along the mesh's own x and z axes (the (u, v, h) derivatives mapped with
    // .xzy) and crossed in that order so a flat surface gives +Y — the plane's rest
    // normal — which is also what it falls back to when uWaveAmplitude is 0.
    vec3 tangentX = dPdu.xzy;
    vec3 tangentZ = dPdv.xzy;
    vec3 waveNormal = normalize(cross(tangentZ, tangentX));

    vec3 N = normalize(normalMat * waveNormal);
    vec3 T = normalize(rotOnly * aTangent.xyz);
    T      = normalize(T - dot(T, N) * N);
    vec3 B = cross(N, T) * aTangent.w;

    vT = T;
    vB = B;
    vN = N;

    gl_Position = uProjection * uView * worldPos;
}
