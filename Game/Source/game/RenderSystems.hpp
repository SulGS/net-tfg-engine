#pragma once
#include <unordered_set>

#include "netcode/netcode_common.hpp"
#include "ecs/ecs_common.hpp"
#include "ecs/UI/UIButton.hpp"
#include "ecs/UI/UIImage.hpp"
#include "ecs/UI/UIText.hpp"
#include "ecs/UI/UIElement.hpp"
#include "Components.hpp"
#include "GameState.hpp"

class CameraFollowSystem : public ISystem
{
public:
    float debugZoomMultiplier = 1.25f; // 1.0 = normal, >1 = zoom out

    void Update(
        EntityManager& entityManager,
        std::vector<EventEntry>& events,
        bool isServer,
        float deltaTime
    ) override
    {

        auto camQuery = entityManager.CreateQuery<Camera, Transform>();
        if (camQuery.Count() == 0)
            return;

        glm::vec3 localPos(0.0f);
        bool foundLocal = false;

        auto playerQuery = entityManager.CreateQuery<Transform, Playable, SpaceShip>();
        for (auto [entity, playerTransform, play, ship] : playerQuery)
        {
            if (play->isLocal && ship->isAlive)
            {
                localPos = playerTransform->getPosition();
                foundLocal = true;

                auto textQuery = entityManager.CreateQuery<UIElement, UIText>();
                for (auto [entity, element, text] : textQuery)
                {
                    element->anchor = UIAnchor::TOP_LEFT;
                    element->position = glm::vec2(0.0f, 0.0f);
                    element->size = glm::vec2(150.0f, 40.0f);
                    element->pivot = glm::vec2(0.0f, 0.0f);

                    text->text = "Health: " + std::to_string(ship->health);
                }
                break;
            }
        }

        if (!foundLocal)
            return;

        for (auto [camEntity, cam, camTrans] : camQuery)
        {
            glm::vec3 newCamPos(
                localPos.x,
                localPos.y - 27.0f * debugZoomMultiplier,
                18.0f * debugZoomMultiplier
            );
            camTrans->setPosition(newCamPos);
            cam->setTarget(localPos);
            cam->markViewDirty();
        }
    }
};


class OnDeathRenderSystem : public ISystem
{
public:
    void Update(
        EntityManager& entityManager,
        std::vector<EventEntry>& events,
        bool isServer,
        float deltaTime
    ) override
    {
        auto playerQuery =
            entityManager.CreateQuery<Transform, Playable, SpaceShip, MeshComponent>();

        for (auto [entity, playerTransform, play, ship, meshC] : playerQuery)
        {
            if (play->isLocal && !ship->isAlive)
            {
                auto textQuery = entityManager.CreateQuery<UIElement, UIText>();

                for (auto [uiEntity, element, text] : textQuery)
                {
                    element->anchor = UIAnchor::CENTER;
                    element->position = glm::vec2(0.0f);
                    element->size = glm::vec2(150.0f, 40.0f);
                    element->pivot = glm::vec2(0.5f);

                    int secondsRemain =
                        std::max(0, ship->deathCooldown / TICKS_PER_SECOND);

                    text->text = std::to_string(secondsRemain + 1);
                }
            }


            if (!ship->isAlive)
            {
                meshC->enabled = false;
            }
            else
            {
                meshC->enabled = true;
            }
        }
    }
};

class ChargingBulletRenderSystem : public ISystem
{
public:
    void Update(
        EntityManager& entityManager,
        std::vector<EventEntry>& events,
        bool isServer,
        float deltaTime
    ) override
    {
        auto playerQuery =
            entityManager.CreateQuery<Transform, Playable, SpaceShip>();

        auto effectQuery =
            entityManager.CreateQuery<Transform, ChargingShootEffect, MeshComponent>();

        const float CHARGING_BULLET_FRAMES = 5;

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
                        effectTransform->setPosition(
                            glm::vec3(
                                playerTransform->getPosition().x,
                                playerTransform->getPosition().y,
                                0.0f
                            )
                        );

                        float scale = (CHARGING_BULLET_FRAMES - ship->remainingShootFrames + 1) * 1.5f;

                        effectTransform->setScale(
                            glm::vec3(scale, scale, 1.0f)
                        );
                    }
                }
            }

            if (!foundIt && ship->isShooting)
            {
                Entity effectEntity = entityManager.CreateEntity();

                Transform* effectTransform =
                    entityManager.AddComponent<Transform>(effectEntity, Transform{});

                effectTransform->setPosition(
                    glm::vec3(
                        playerTransform->getPosition().x,
                        playerTransform->getPosition().y,
                        0.0f
                    )
                );

                float scale =
                    (CHARGING_BULLET_FRAMES - ship->remainingShootFrames + 1) * 1.5f;

                effectTransform->setScale(
                    glm::vec3(scale, scale, 1.0f)
                );

                ChargingShootEffect* effectComponent =
                    entityManager.AddComponent<ChargingShootEffect>(
                        effectEntity,
                        ChargingShootEffect{}
                    );

                effectComponent->entity = play->playerId;

                auto shootingMat =
                    std::make_shared<Material>("generic.vert", "generic.frag");

                shootingMat->setVec3("uColor", glm::vec3(1.0f, 1.0f, 0.0f));

                entityManager.AddComponent<MeshComponent>(
                    effectEntity,
                    MeshComponent(new Mesh("charge.glb", shootingMat))
                );
            }
        }
    }
};

class DestroyTimerSystem : public ISystem
{
public:
    void Update(
        EntityManager& entityManager,
        std::vector<EventEntry>& events,
        bool isServer,
        float deltaTime
    ) override
    {
        auto query = entityManager.CreateQuery<DestroyTimer>();

        for (auto [entity, destroyTimer] : query)
        {
            auto audio = entityManager.GetComponent<AudioSourceComponent>(entity);
            if (audio->pendingToDestroy) continue;
            destroyTimer->framesRemaining -= 1;

            if (destroyTimer->framesRemaining <= 0)
            {

                audio->pendingToDestroy = true;
            }
        }
    }
};


class LinkThrusterToShipSystem : public ISystem
{
public:
    void Update(
        EntityManager& entityManager,
        std::vector<EventEntry>& events,
        bool isServer,
        float deltaTime
    ) override
    {
        auto thrusterQuery = entityManager.CreateQuery<Transform, ParticleEmitterComponent, ThrusterOwner>();
        auto shipQuery = entityManager.CreateQuery<Transform, Playable, SpaceShip>();

        //Rotate ship

        for (auto [shipEntity, shipTransform, play, ship] : shipQuery)
        {
            if (!ship->isAlive) continue;
            float yawRad = glm::radians(shipTransform->getRotation().z);
            float incl = ship->shipInclination * -1.0f;
            glm::vec3 rotation;
            rotation.x = incl * cos(yawRad);
            rotation.y = incl * sin(yawRad);
            rotation.z = shipTransform->getRotation().z;
            shipTransform->setRotation(rotation);
        }

        for (auto [thrusterEntity, thrusterTransform, thrusterEmitter, thrusterOwner] : thrusterQuery)
        {
            for (auto [shipEntity, shipTransform, play, ship] : shipQuery)
            {
                if (thrusterOwner->shipEntity != play->playerId) continue;

                // --- Position (same for both thruster and smoke) ---
                glm::vec3 localOffset = glm::vec3(-1.8f, 0.0f, 0.0f);

                if (thrusterOwner->isLeftEngine)
                {
                    localOffset.y = -0.75f;
                }
                else
                {
                    localOffset.y = 0.75f;
                }




                glm::mat4 model = shipTransform->getModelMatrix();
                glm::mat3 rot = glm::mat3(model);
                rot[0] = glm::normalize(rot[0]);
                rot[1] = glm::normalize(rot[1]);
                rot[2] = glm::normalize(rot[2]);
                thrusterTransform->setPosition(shipTransform->getPosition() + rot * localOffset);


                // --- Activation ---
                if (thrusterOwner->isSmoke)
                {
                    thrusterEmitter->enabled = !ship->isMovingForward;  // smoke when idle
                    thrusterTransform->setRotation(glm::vec3(
                        0.0f,
                        shipTransform->getRotation().z,
                        0.0f
                    ));
                }
                else
                {
                    thrusterEmitter->enabled = ship->isMovingForward;   // exhaust when moving
                    thrusterTransform->setRotation(glm::vec3(
                        90.0f,
                        shipTransform->getRotation().z,
                        0.0f
                    ));
                }

            }
        }
    }
};

class LaserWallRenderSystem : public ISystem
{
public:
    void Update(
        EntityManager& entityManager,
        std::vector<EventEntry>& events,
        bool isServer,
        float deltaTime
    ) override
    {
        const int x_size = 5;
        const int y_size = 5;

        // Build active tile set
        std::unordered_set<int> activeTileIds;
        {
            auto tileActiveQuery = entityManager.CreateQuery<TileID>();
            for (auto [entity, tileId] : tileActiveQuery)
                if (tileId->active) activeTileIds.insert(tileId->id);
        }

        // Tiles: active flag drives visibility
        {
            auto tileQuery = entityManager.CreateQuery<TileID, MeshComponent>();
            for (auto [entity, tileId, mesh] : tileQuery)
                mesh->enabled = tileId->active;
        }

        // Walls visibility rules:
        // 1. Owning cell inactive -> always hidden
        // 2. Owning cell active, neighbour inactive/out-of-bounds -> always shown (new border)
        // 3. Both cells active -> driven by lwID->enabled (toggler)
        {
            auto laserWallQuery = entityManager.CreateQuery<LaserWallID, MeshComponent>();
            for (auto [entity, lwID, mesh] : laserWallQuery)
            {
                bool isSpoke = entityManager.GetComponent<CenterSpoke>(entity) != nullptr;

                bool ownerActive = activeTileIds.count(lwID->cellId) > 0;
                if (!ownerActive)
                {
                    mesh->enabled = false;
                    continue;
                }

                if (isSpoke)
                {
                    mesh->enabled = lwID->enabled; // spokes always driven by enabled flag only
                    continue;
                }

                int cx = lwID->cellId / y_size;
                int cy = lwID->cellId % y_size;
                int nx = cx, ny = cy;
                switch (lwID->dir)
                {
                case CellCardinalDirection::Left:  nx--; break;
                case CellCardinalDirection::Right: nx++; break;
                case CellCardinalDirection::Down:  ny--; break;
                case CellCardinalDirection::Up:    ny++; break;
                default: break;
                }

                bool neighbourInactive = nx < 0 || nx >= x_size || ny < 0 || ny >= y_size
                    || !activeTileIds.count(nx * y_size + ny);

                if (neighbourInactive)
                    mesh->enabled = true;
                else
                    mesh->enabled = lwID->enabled;
            }
        }

        // Pillars: hidden when ALL their cells are inactive
        {
            auto pillarQuery = entityManager.CreateQuery<PillarID, MeshComponent>();
            for (auto [entity, pid, mesh] : pillarQuery)
            {
                if (pid->cellsIsIn.empty()) { mesh->enabled = false; continue; }
                bool allGone = true;
                for (int cellId : pid->cellsIsIn)
                    if (activeTileIds.count(cellId)) { allGone = false; break; }
                mesh->enabled = !allGone;
            }
        }
    }
};