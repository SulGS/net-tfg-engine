#ifndef MESH_COMPONENT_HPP
#define MESH_COMPONENT_HPP

// Kept out of ecs/ecs_common.hpp so the shared/headless ECS (and GameServer) never sees Mesh, and through it OpenGL.

#include <memory>
#include "ecs/ecs.hpp"
#include "OpenGL/Mesh.hpp"

class MeshComponent : public IComponent {
public:
    std::unique_ptr<Mesh> mesh;
    bool enabled;
	bool castShadows = true;
	// Light-only mesh (beams, glows). Skipped by GBuffer/shadow/opaque passes and summed by RenderSystem::AdditivePass:
	// (SRC_ALPHA, ONE), depth test without write, so order doesn't matter. Unlit shader: only uCameraPos and
	// uModel/uView/uProjection, no texture units bound.
	bool additive = false;
	// Drawn into reflection probe captures (see ReflectionProbeComponent). Turn off for anything that moves.
	bool reflectionProbeVisible = true;
    MeshComponent() : mesh(nullptr), enabled(true), castShadows(true) {}
    MeshComponent(Mesh* m) : mesh(m), enabled(true), castShadows(true) {}
};

#endif // MESH_COMPONENT_HPP
