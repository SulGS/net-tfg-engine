#pragma once

#include <algorithm>

#include "ecs/ecs_common.hpp"
#include "../Components.hpp"
#include "LaserVisuals.hpp"

// Per-frame laser bolt state: laser_bolt.frag uniforms (render clock, age for spawn flash/ramp-in, fade so it dissipates)
// and its point light intensity (same age/fade). Purely visual, never synced/predicted; position/heading set at creation.
class BulletRenderSystem : public ISystem
{
    const float FADE_TICKS = 6.0f; // the bolt dissipates over the last N ticks of its lifetime
    // The bolt is born right at the nose/hull edge, where a full-strength light
    // would blow it out for a frame or two, so the light swells in over this long instead.
    const float LIGHT_RAMP_SECONDS = 0.08f;
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

        auto bulletQuery = entityManager.CreateQuery<ECSBullet, MeshComponent, BulletVisual>();
        for (auto [entity, bullet, mesh, visual] : bulletQuery)
        {
            visual->age += deltaTime;

            const float fade = std::clamp(bullet->lifetime / FADE_TICKS, 0.0f, 1.0f);

            if (PointLightComponent* light = entityManager.GetComponent<PointLightComponent>(entity))
            {
                const float ramp = std::clamp(visual->age / LIGHT_RAMP_SECONDS, 0.0f, 1.0f);
                light->intensity = LASER_BOLT_LIGHT_INTENSITY * ramp * ramp * (3.0f - 2.0f * ramp) * fade;
            }

            if (!mesh->mesh) continue;
            if (Material* mat = mesh->mesh->getMaterial())
            {
                mat->setFloat("uTime", time);
                mat->setFloat("uAge", visual->age);
                mat->setFloat("uFade", fade);
            }
        }
    }
};
