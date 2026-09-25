#version 430 core

// Pairs with laser_wall.frag. laser_beam.glb is a tube along local Z (-1..1) narrowing towards both ends. Its normals
// are useless for the beam profile, so this exports what the fragment shader needs to rebuild it: beam axis and radial
// direction (world space) plus the distance along the beam in world units.

layout(location = 0) in vec3 aPos;

uniform mat4 uModel;
uniform mat4 uView;
uniform mat4 uProjection;

out vec3  vWorldPos;
out vec3  vAxisW;    // beam axis (unit, world space)
out vec3  vRadialW;  // axis -> surface direction (world space, not normalised)
out float vAlong;    // world-space distance from the beam centre along the axis
out float vEnd;      // -1..1 along the beam, model space (0 = middle, +-1 = an end)

void main()
{
    vec4 worldPos = uModel * vec4(aPos, 1.0);
    vWorldPos     = worldPos.xyz;

    // Strip scale per column (same reasoning as the default vertex shader in DefaultShader.hpp) to get a pure rotation.
    mat3 modelMat = mat3(uModel);
    mat3 rotOnly  = mat3(
        modelMat[0] / length(modelMat[0]),
        modelMat[1] / length(modelMat[1]),
        modelMat[2] / length(modelMat[2])
    );

    vAxisW   = rotOnly[2];
    vRadialW = rotOnly * vec3(aPos.xy, 0.0);
    vAlong   = aPos.z * length(modelMat[2]);
    vEnd     = aPos.z;

    gl_Position = uProjection * uView * worldPos;
}
