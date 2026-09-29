#pragma once

#include "ecs/ecs_common.hpp"
#include "../Components.hpp"

// Drives "uTime" for every FluidSurface mesh (fluid.vert waves, water/lava.frag detail). Render-side clock only:
// not synced or predicted, each client animates on its own timeline.
class FluidAnimationSystem : public ISystem
{
	float time = 0.0f;

public:
	void Update(
		EntityManager& entityManager,
		std::vector<EventEntry>& events,
		bool isServer,
		float deltaTime
	) override
	{
		time += deltaTime;

		auto query = entityManager.CreateQuery<MeshComponent, FluidSurface>();
		for (auto [entity, meshC, fluidTag] : query)
		{
			if (!meshC->mesh) continue;
			if (Material* mat = meshC->mesh->getMaterial())
				mat->setFloat("uTime", time);
		}
	}
};
