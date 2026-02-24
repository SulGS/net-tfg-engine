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
out mat3 vTBN;

void main()
{
    vec4 worldPos = uModel * vec4(aPos, 1.0);
    vWorldPos     = worldPos.xyz;
    vUV           = aUV;

    mat3 normalMatrix = mat3(transpose(inverse(uModel)));

    vec3 N = normalize(normalMatrix * aNormal);
    vec3 T = normalize(normalMatrix * aTangent.xyz);
    T      = normalize(T - dot(T, N) * N); // Gram-Schmidt re-orthogonalize
    vec3 B = cross(N, T) * aTangent.w;     // w = bitangent handedness

    vTBN = mat3(T, B, N);

    gl_Position = uProjection * uView * worldPos;
}