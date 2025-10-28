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
    void Update(EntityManager& entityManager, float deltaTime) override {
        // Query cameras with their transforms
        auto camQuery = entityManager.CreateQuery<Camera, Transform>();
        if (camQuery.Count() == 0) return; // No camera found

        // Find the local player position (if any)
        glm::vec3 localPos(0.0f);
        bool foundLocal = false;
        auto playerQuery = entityManager.CreateQuery<Transform, Playable, SpaceShip>();

        for (auto [entity, playerTransform, play, ship] : playerQuery) {
            if (play->isLocal) {
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
    void Update(EntityManager& entityManager, float deltaTime) override {
        bool foundLocal = false;
        auto playerQuery = entityManager.CreateQuery<Transform, Playable, SpaceShip, MeshComponent>();

        for (auto [entity, playerTransform, play, ship, meshC] : playerQuery) {
            if (play->isLocal) {
                if (ship->health == 0 && ship->deathCooldown > 0)
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

            if (ship->health <= 0)
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