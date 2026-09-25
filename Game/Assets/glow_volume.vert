#version 430 core

// Shared vertex shader for the volumetric glows (laser_bolt.frag, laser_charge.frag). The mesh is a UNIT SPHERE canvas
// (charge.glb) stretched by the entity transform. The fragment shader raymarches in LOCAL space, where the canvas is
// always the unit sphere, so it only needs the fragment's local position and the camera position in that space.

layout(location = 0) in vec3 aPos;

uniform mat4 uModel;
uniform mat4 uView;
uniform mat4 uProjection;
uniform vec3 uCameraPos; // world space, set by RenderSystem::AdditivePass

out vec3 vLocalPos; // point on the canvas sphere, unit-sphere space
out vec3 vCamLocal; // camera in the same space (constant across the mesh)

void main()
{
    vLocalPos = aPos;
    vCamLocal = (inverse(uModel) * vec4(uCameraPos, 1.0)).xyz;
    gl_Position = uProjection * uView * uModel * vec4(aPos, 1.0);
}
