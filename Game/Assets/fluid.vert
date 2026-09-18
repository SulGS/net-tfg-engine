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

        // Vertical bob plus a small horizontal pinch toward the wave crest —
        // the pinch is what gives Gerstner waves their sharper, choppier
        // crest shape instead of a plain sine's smooth round top.
        offset.z += a * s;
        offset.x += -d.x * a * c;
        offset.y += -d.y * a * c;

        float dS = a * k * c;
        dPdx += vec3(-d.x * d.x * a * k * s, -d.x * d.y * a * k * s, d.x * dS);
        dPdy += vec3(-d.x * d.y * a * k * s, -d.y * d.y * a * k * s, d.y * dS);
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
    mat3 modelMat = mat3(uModel);
    mat3 rotOnly  = mat3(
        modelMat[0] / length(modelMat[0]),
        modelMat[1] / length(modelMat[1]),
        modelMat[2] / length(modelMat[2])
    );

    // Analytic normal of the displaced surface replaces aNormal; falls back
    // to it gracefully since dPdx/dPdy start at the flat basis (1,0,0)/(0,1,0)
    // when uWaveAmplitude is 0.
    vec3 waveNormal = normalize(cross(dPdx, dPdy));

    vec3 N = normalize(rotOnly * waveNormal);
    vec3 T = normalize(rotOnly * aTangent.xyz);
    T      = normalize(T - dot(T, N) * N);
    vec3 B = cross(N, T) * aTangent.w;

    vT = T;
    vB = B;
    vN = N;

    gl_Position = uProjection * uView * worldPos;
}
