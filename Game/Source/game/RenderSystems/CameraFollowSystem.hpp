#pragma once

#include <algorithm>
#include <string>
#include <vector>

#include "ecs/ecs_common.hpp"
#include "ecs/UI/UIButton.hpp"
#include "ecs/UI/UIElement.hpp"
#include "ecs/UI/UIText.hpp"
#include "Utils/Input.hpp"
#include "../Components.hpp"
#include "GameResult.hpp"

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
        auto buttonQuery = entityManager.CreateQuery<UIElement, UIButton, ExitButtonChecker>();

        for (auto [entity, element, button, exitChecker] : buttonQuery)
        {
            element->isVisible = false;
        }

        auto camQuery = entityManager.CreateQuery<Camera, Transform>();
        if (camQuery.Count() == 0)
            return;

        bool localAlive = false;
        bool localFound = false;
        int  localPlayerId = -1;
        glm::vec3 localPos(0.0f);

        auto playerQuery = entityManager.CreateQuery<Transform, Playable, SpaceShip>();
        for (auto [entity, playerTransform, play, ship] : playerQuery)
        {
            if (play->isLocal)
            {
                localFound = true;
                localPlayerId = play->playerId;
                localAlive = ship->isAlive;
                localPos = playerTransform->getPosition();
                break;
            }
        }

        if (!localFound)
            return;

        glm::vec3 targetPos(0.0f);

        // Game over: all players see the winner zoom regardless of alive state
        int winnerId = GetWinnerId(entityManager);
        bool gameOver = (winnerId >= 0);

        if (gameOver)
        {
            for (auto [e2, pt2, pl2, sh2] : playerQuery)
            {
                if (pl2->playerId == winnerId)
                {
                    targetPos = pt2->getPosition();
                    break;
                }
            }

            // Update UI for the alive winner (dead players are handled by OnDeathRenderSystem)
            if (localAlive)
            {
                auto textQuery = entityManager.CreateQuery<UIElement, UIText, GameStatusText>();
                for (auto [entity, element, text, statusTag] : textQuery)
                {
                    element->anchor = UIAnchor::TOP_CENTER;
                    element->position = glm::vec2(0.0f, 20.0f);
                    element->size = glm::vec2(350.0f, 80.0f);
                    element->pivot = glm::vec2(0.5f);
                    text->text = "PLAYER " + std::to_string(winnerId + 1) + " WINS";
                }

				auto buttonQuery = entityManager.CreateQuery<UIElement, UIButton, ExitButtonChecker>();

				for (auto [entity, element, button, exitChecker] : buttonQuery)
				{
					element->isVisible = true;
				}
            }

            float zoom = 0.6f;
            for (auto [camEntity, cam, camTrans] : camQuery)
            {
                glm::vec3 newCamPos(
                    targetPos.x,
                    targetPos.y - 27.0f * zoom,
                    18.0f * zoom
                );
                camTrans->setPosition(newCamPos);
                cam->setTarget(targetPos);
                cam->markViewDirty();
            }
            return;
        }

        // Normal gameplay
        if (localAlive)
        {
            targetPos = localPos;

            int aliveCount = 0;
            for (auto [e2, pt2, pl2, sh2] : playerQuery)
                if (sh2->isAlive) aliveCount++;

            auto textQuery = entityManager.CreateQuery<UIElement, UIText, GameStatusText>();
            for (auto [entity, element, text, statusTag] : textQuery)
            {
                element->anchor = UIAnchor::TOP_LEFT;
                element->position = glm::vec2(0.0f, 0.0f);
                element->size = glm::vec2(300.0f, 40.0f);
                element->pivot = glm::vec2(0.0f, 0.0f);
                text->text = "REMAINING: " + std::to_string(aliveCount);
            }
        }
        else
        {
            // Spectating: find the SpectatorState and follow the watched player
            SpectatorState* spectator = nullptr;
            for (auto [entity, playerTransform, play, ship] : playerQuery)
            {
                if (play->isLocal)
                {
                    spectator = entityManager.GetComponent<SpectatorState>(entity);

                    // Lazily add SpectatorState if missing
                    if (!spectator)
                    {
                        int firstAlive = -1;
                        for (auto [e2, pt2, pl2, sh2] : playerQuery)
                        {
                            if (pl2->playerId != localPlayerId && sh2->isAlive)
                            {
                                firstAlive = pl2->playerId;
                                break;
                            }
                        }
                        SpectatorState newState;
                        newState.watchedPlayerId = (firstAlive >= 0) ? firstAlive : localPlayerId;
                        spectator = entityManager.AddComponent<SpectatorState>(entity, newState);
                    }

                    // Cycle target with LEFT / RIGHT arrow keys — read hardware
                    // directly here since this is renderer-only local state and
                    // must never touch the networked input blob.
                    bool leftNow = Input::KeyPressed(Input::ArrowLeft);
                    bool rightNow = Input::KeyPressed(Input::ArrowRight);

                    auto cycleTarget = [&](int direction)
                        {
                            std::vector<int> alive;
                            for (auto [e2, pt2, pl2, sh2] : playerQuery)
                                if (pl2->playerId != localPlayerId && sh2->isAlive)
                                    alive.push_back(pl2->playerId);

                            if (alive.empty()) return;

                            auto it = std::find(alive.begin(), alive.end(), spectator->watchedPlayerId);
                            int idx = (it != alive.end()) ? (int)(it - alive.begin()) : 0;
                            idx = (idx + direction + (int)alive.size()) % (int)alive.size();
                            spectator->watchedPlayerId = alive[idx];
                        };

                    if (leftNow && !spectator->prevLeftHeld)  cycleTarget(-1);
                    if (rightNow && !spectator->prevRightHeld) cycleTarget(+1);

                    spectator->prevLeftHeld = leftNow;
                    spectator->prevRightHeld = rightNow;

                    break;
                }
            }

            // Follow the watched player (or stay put if everyone is dead)
            bool watchedFound = false;
            if (spectator)
            {
                for (auto [entity, playerTransform, play, ship] : playerQuery)
                {
                    if (play->playerId == spectator->watchedPlayerId)
                    {
                        targetPos = playerTransform->getPosition();
                        watchedFound = true;
                        break;
                    }
                }
            }
            if (!watchedFound)
                targetPos = localPos;
        }

        for (auto [camEntity, cam, camTrans] : camQuery)
        {
            glm::vec3 newCamPos(
                targetPos.x,
                targetPos.y - 27.0f * debugZoomMultiplier,
                18.0f * debugZoomMultiplier
            );
            camTrans->setPosition(newCamPos);
            cam->setTarget(targetPos);
            cam->markViewDirty();
        }
    }
};
