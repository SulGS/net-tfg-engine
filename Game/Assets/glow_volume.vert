#version 430 core

// Shared vertex shader for the volumetric energy effects (laser_bolt.frag,
// laser_charge.frag). The mesh is only a bounding canvas: a UNIT SPHERE (charge.glb)
// that the entity's transform stretches into whatever volume the effect needs
// (a long ellipsoid for a bolt, a sphere for the charge orb).
//
// The fragment shader raymarches the glow inside that volume in the mesh's LOCAL
// space, where the canvas is always the unit sphere no matter how it is scaled or
// rotated. So all it needs from here is the fragment's local position and the
// camera's position in the same space.

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
