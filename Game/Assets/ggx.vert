#version 430 core

layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aUV;
layout(location = 3) in vec4 aTangent; // xyz = tangent, w = bitangent sign

uniform mat4 uModel;
uniform mat4 uView;
uniform mat4 uProjection;

out vec3 vWorldPos;
out vec2 vUV;
out vec3 vT;
out vec3 vB;
out vec3 vN;

void main()
{
    vec4 worldPos = uModel * vec4(aPos, 1.0);
    vWorldPos     = worldPos.xyz;
    vUV           = aUV;

    // Extract rotation-only from uModel by stripping scale from each column.
    // Numerically stable at any uniform scale (e.g. 200): all values stay near
    // 1.0, unlike transpose(inverse(uModel)) which produces values of 1/scale
    // (0.005 at scale 200) causing precision loss in interpolated TBN varyings
    // across large triangles — the root cause of the specular dot/ring artifact.
    mat3 modelMat = mat3(uModel);
    mat3 rotOnly  = mat3(
        modelMat[0] / length(modelMat[0]),
        modelMat[1] / length(modelMat[1]),
        modelMat[2] / length(modelMat[2])
    );

    vec3 N = normalize(rotOnly * aNormal);
    vec3 T = normalize(rotOnly * aTangent.xyz);
    T      = normalize(T - dot(T, N) * N); // Gram-Schmidt re-orthogonalize
    vec3 B = cross(N, T) * aTangent.w;     // w = bitangent handedness

    vT = T;
    vB = B;
    vN = N;

    gl_Position = uProjection * uView * worldPos;
}
