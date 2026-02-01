#pragma once

#include "netcode/netcode_common.hpp"
#include "ecs/ecs_common.hpp"

#include "ecs/UI/UIButton.hpp"
#include "ecs/UI/UIImage.hpp"
#include "ecs/UI/UIText.hpp"
#include "ecs/UI/UIElement.hpp"

#include "Components.hpp"
#include "GameState.hpp"

class CameraFollowSystem : public ISystem {
public:
    void Update(EntityManager& entityManager, std::vector<EventEntry>& events,bool isServer, float deltaTime) override {
        // Query cameras with their transforms
        auto camQuery = entityManager.CreateQuery<Camera, Transform>();
        if (camQuery.Count() == 0) return; // No camera found

        // Find the local player position (if any)
        glm::vec3 localPos(0.0f);
        bool foundLocal = false;
        auto playerQuery = entityManager.CreateQuery<Transform, Playable, SpaceShip>();

        for (auto [entity, playerTransform, play, ship] : playerQuery) {
            if (play->isLocal && ship->isAlive) {
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

        // Update each camera to follow the local player smoothly
        const float followSpeed = 8.0f; // higher = snappier, lower = smoother
        for (auto [camEntity, cam, camTrans] : camQuery) {
            if (!foundLocal) break; // nothing to follow

            // Desired camera position preserves Z but matches player's X/Y
            glm::vec3 desiredPos(localPos.x, localPos.y, camTrans->getPosition().z);

            // Let Transform perform smoothing (same exponential smoothing math)
            camTrans->SmoothPositionToward(desiredPos, deltaTime, followSpeed);

            // Smooth camera target toward the player
            glm::vec3 currentTarget = cam->getTarget();
            float t = 1.0f - std::exp(-followSpeed * deltaTime);
            glm::vec3 newTarget = currentTarget + (localPos - currentTarget) * t;
            cam->setTarget(newTarget);
            cam->markViewDirty();
        }
    }
};



class OnDeathRenderSystem : public ISystem {
public:
    void Update(EntityManager& entityManager, std::vector<EventEntry>& events, bool isServer, float deltaTime) override {
        bool foundLocal = false;
        auto playerQuery = entityManager.CreateQuery<Transform, Playable, SpaceShip, MeshComponent>();

        for (auto [entity, playerTransform, play, ship, meshC] : playerQuery) {
            if (play->isLocal) {
                if (!(ship->isAlive))
                {
                    auto textQuery = entityManager.CreateQuery<UIElement, UIText>();

                    for (auto [entity, element, text] : textQuery)
                    {
                        element->anchor = UIAnchor::CENTER;
                        element->position = glm::vec2(0.0f, 0.0f);
                        element->size = glm::vec2(150.0f, 40.0f);
                        element->pivot = glm::vec2(0.5f, 0.5f);

                        int seconds_remain = ship->deathCooldown / TICKS_PER_SECOND;



                        text->text = std::to_string(seconds_remain + 1);
                    }
                }

            }

            if (!(ship->isAlive))
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

std::vector<float> CircleVerts(float radius, int segments = 32) {
    // Centered circle at origin
    std::vector<float> vertices;

    for (int i = 0; i < segments; ++i) {
        float angle = 2.0f * glm::pi<float>() * i / segments;
        float x = radius * cos(angle);
        float y = radius * sin(angle);
        vertices.push_back(x);
        vertices.push_back(y);
        vertices.push_back(0.0f);
    }

    return vertices;
}

class ChargingBulletRenderSystem : public ISystem {
public:
    void Update(EntityManager& entityManager, std::vector<EventEntry>& events, bool isServer, float deltaTime) override {
        bool foundLocal = false;
        auto playerQuery = entityManager.CreateQuery<Transform, Playable, SpaceShip>();
        auto effectQuery = entityManager.CreateQuery<Transform, ChargingShootEffect, MeshComponent>();

        const float CHARGING_BULLET_FRAMES = 5;

        for (auto [playerEntity, playerTransform, play, ship] : playerQuery) {
                bool foundIt = false;

                for (auto [effectEntity, effectTransform, effect, mesh] : effectQuery) 
                {
                    if (effect->entity == play->playerId) 
                    {
                        foundIt = true;
                        if (ship->remainingShootFrames == -1) 
                        {
                            entityManager.DestroyEntity(effectEntity);
                        }
                        else 
                        {
                            effectTransform->setPosition(glm::vec3(playerTransform->getPosition().x, playerTransform->getPosition().y, 0));
                            effectTransform->setScale(glm::vec3((CHARGING_BULLET_FRAMES - ship->remainingShootFrames + 1) * 1.5f,
                                                                    (CHARGING_BULLET_FRAMES - ship->remainingShootFrames + 1) * 1.5f,
                                                                    1));
                        }
                    }
                }

                if (!foundIt && ship->remainingShootFrames != -1) 
                {
                    Entity effectEntity = entityManager.CreateEntity();
                    Transform* effectTransform = entityManager.AddComponent<Transform>(effectEntity, Transform{});
                    effectTransform->setPosition(glm::vec3(playerTransform->getPosition().x, playerTransform->getPosition().y, 0));
                    effectTransform->setScale(glm::vec3((CHARGING_BULLET_FRAMES - ship->remainingShootFrames + 1) * 1.5f,
                                                            (CHARGING_BULLET_FRAMES - ship->remainingShootFrames + 1) * 1.5f,
                                                            1));
                    ChargingShootEffect* effectComponent = entityManager.AddComponent<ChargingShootEffect>(effectEntity, ChargingShootEffect{});
                    effectComponent->entity = play->playerId;

                    std::vector<unsigned int> circleInds = {
                                                0, 1, 2,
                                                0, 2, 3,
                                                0, 3, 4,
                                                0, 4, 5,
                                                0, 5, 6,
                                                0, 6, 7,
                                                0, 7, 8,
                                                0, 8, 9,
                                                0, 9, 10,
                                                0, 10, 11,
                                                0, 11, 12,
                                                0, 12, 13,
                                                0, 13, 14,
                                                0, 14, 15,
                                                0, 15, 16,
                                                0, 16, 17,
                                                0, 17, 18,
                                                0, 18, 19,
                                                0, 19, 20,
                                                0, 20, 21,
                                                0, 21, 22,
                                                0, 22, 23,
                                                0, 23, 24,
                                                0, 24, 25,
                                                0, 25, 26,
                                                0, 26, 27,
                                                0, 27, 28,
                                                0, 28, 29,
                                                0, 29, 30,
                                                0, 30, 31,
                                                0, 31, 1
                                                                };

                    auto shootingMat = std::make_shared<Material>("generic.vert", "generic.frag");
                    shootingMat->setVec3("uColor", glm::vec3(1.0f, 1.0f, 0.0f));
                    entityManager.AddComponent<MeshComponent>(effectEntity, MeshComponent(new Mesh(CircleVerts(1), circleInds, shootingMat)));

                }

            
        }
    }
};

class DestroyTimerSystem : public ISystem {
public:
	void Update(EntityManager& entityManager, std::vector<EventEntry>& events, bool isServer, float deltaTime) override {
		auto query = entityManager.CreateQuery<DestroyTimer>();
		for (auto [entity, destroyTimer] : query) {
			destroyTimer->framesRemaining -= 1;
			if (destroyTimer->framesRemaining <= 0) {
				entityManager.DestroyEntity(entity);
			}
		}
	}
};