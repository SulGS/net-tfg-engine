#pragma once

#include <algorithm>
#include <cmath>

#include "ecs/ecs_common.hpp"
#include "../Components.hpp"
#include "LaserVisuals.hpp"

// Charge-up before a shot (laser_charge.frag): an orb at the muzzle while charging, sparks contracting into a
// white-hot core. Created when the charge starts, destroyed when it ends (the bolt is BulletRenderSystem's job).
class ChargingBulletRenderSystem : public ISystem
{
    const float CHARGING_BULLET_FRAMES = 5; // = CHARGE_SHOOT_FRAMES in InputSystem
    float time = 0.0f; // render-side clock for the shader ("uTime")

    // Puts the orb on the ship's nose and pushes the charge state to its shader.
    void UpdateOrb(Transform* orbTransform, MeshComponent* orbMesh,
        Transform* shipTransform, const SpaceShip* ship) const
    {
        // Same heading the bullet will be fired along (see InputServerSystem).
        const float yaw = glm::radians(shipTransform->getRotation().z);
        const glm::vec3 shipPos = shipTransform->getPosition();
        orbTransform->setPosition(glm::vec3(
            shipPos.x + std::cos(yaw) * SHIP_MUZZLE_OFFSET,
            shipPos.y + std::sin(yaw) * SHIP_MUZZLE_OFFSET,
            0.0f));

        // 0.2 on the first frame of the charge, 1.0 on the last.
        const float progress = std::clamp(
            (CHARGING_BULLET_FRAMES - ship->remainingShootFrames + 1) / CHARGING_BULLET_FRAMES,
            0.0f, 1.0f);

        if (!orbMesh->mesh) return;
        if (Material* mat = orbMesh->mesh->getMaterial())
        {
            mat->setFloat("uTime", time);
            mat->setFloat("uProgress", progress);
        }
    }

public:
    void Update(
        EntityManager& entityManager,
        std::vector<EventEntry>& events,
        bool isServer,
        float deltaTime
    ) override
    {
        time += deltaTime;

        auto playerQuery =
            entityManager.CreateQuery<Transform, Playable, SpaceShip>();

        auto effectQuery =
            entityManager.CreateQuery<Transform, ChargingShootEffect, MeshComponent>();

        for (auto [playerEntity, playerTransform, play, ship] : playerQuery)
        {
            bool foundIt = false;

            for (auto [effectEntity, effectTransform, effect, mesh] : effectQuery)
            {
                if (effect->entity == play->playerId)
                {
                    foundIt = true;

                    if (!ship->isShooting)
                    {
                        entityManager.DestroyEntity(effectEntity);
                    }
                    else
                    {
                        UpdateOrb(effectTransform, mesh, playerTransform, ship);
                    }
                }
            }

            if (!foundIt && ship->isShooting)
            {
                Entity effectEntity = entityManager.CreateEntity();

                Transform* effectTransform =
                    entityManager.AddComponent<Transform>(effectEntity, Transform{});

                // Constant size: the charge builds through the shader, not the scale.
                effectTransform->setScale(glm::vec3(CHARGE_ORB_RADIUS));

                ChargingShootEffect* effectComponent =
                    entityManager.AddComponent<ChargingShootEffect>(
                        effectEntity,
                        ChargingShootEffect{}
                    );

                effectComponent->entity = play->playerId;

                MeshComponent* orbMesh =
                    AddChargeOrbMesh(entityManager, effectEntity, play->playerId);

                UpdateOrb(effectTransform, orbMesh, playerTransform, ship);
            }
        }
    }
};
