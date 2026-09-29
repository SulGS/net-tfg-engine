#pragma once

#include <string>

#include "ecs/ecs_common.hpp"
#include "ecs/UI/UIButton.hpp"
#include "ecs/UI/UIElement.hpp"
#include "ecs/UI/UIText.hpp"
#include "OpenAL/AudioComponents.hpp"
#include "OpenGL/Particles/ParticleEmitterComponent.hpp"
#include "../Components.hpp"
#include "../Explosion.hpp"
#include "GameResult.hpp"

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
            entityManager.CreateQuery<Transform, Playable, SpaceShip, MeshComponent, JustDeathChecker>();

        for (auto [entity, playerTransform, play, ship, meshC, jdC] : playerQuery)
        {

            if (!ship->isAlive)
            {
                if (jdC->notExecuted) {

                    jdC->notExecuted = false;


                    SpawnShipExplosion(entityManager, playerTransform->getPosition(), play->playerId);

                    Entity audioEntity = entityManager.CreateEntity();
					Transform* audioTransform = entityManager.AddComponent<Transform>(audioEntity, Transform{});
					audioTransform->setPosition(playerTransform->getPosition());
                    AudioSourceComponent* audio = entityManager.AddComponent<AudioSourceComponent>(
                        audioEntity, AudioSourceComponent("explosion.wav", AudioChannel::SFX, false));
					audio->gain = 1.0f;
                    audio->play = true;
                    entityManager.AddComponent<ExplosionPlayerID>(audioEntity, ExplosionPlayerID{ play->playerId });
                }
            }
            else
            {
				jdC->notExecuted = true; // reset for potential future deaths

				auto explosionQuery = entityManager.CreateQuery<Transform, ParticleEmitterComponent, ExplosionPlayerID>();
                for (auto [e, t, emitter, expID] : explosionQuery)
                {
                    if (expID->playerId == play->playerId)
                    {
                        entityManager.DestroyEntity(e);
                    }
                }

				auto audioQuery = entityManager.CreateQuery<AudioSourceComponent, ExplosionPlayerID>();
				for (auto [e, audio, expID] : audioQuery)
				{
					if (expID->playerId == play->playerId)
					{
						audio->pendingToDestroy = true;
					}
				}
            }


            if (play->isLocal && !ship->isAlive)
            {

                int winnerId = GetWinnerId(entityManager);
                bool gameOver = (winnerId >= 0);

                // Find who we're spectating (only needed when game is still ongoing)
                std::string watchedName = "";
                if (!gameOver)
                {
                    SpectatorState* spectator = entityManager.GetComponent<SpectatorState>(entity);
                    if (spectator)
                    {
                        auto allPlayers = entityManager.CreateQuery<Playable, SpaceShip>();
                        for (auto [e2, pl2, sh2] : allPlayers)
                        {
                            if (pl2->playerId == spectator->watchedPlayerId)
                            {
                                watchedName = "Player " + std::to_string(spectator->watchedPlayerId + 1);
                                break;
                            }
                        }
                    }
                }

                auto buttonQuery = entityManager.CreateQuery<UIElement, UIButton, ExitButtonChecker>();

                for (auto [entity, element, button, exitChecker] : buttonQuery)
                {
                    element->isVisible = true;
                }

                auto textQuery = entityManager.CreateQuery<UIElement, UIText, GameStatusText>();
                for (auto [uiEntity, element, text, statusTag] : textQuery)
                {
                    element->anchor = UIAnchor::TOP_CENTER;
                    element->position = glm::vec2(0.0f, 20.0f);
                    element->size = glm::vec2(350.0f, 80.0f);
                    element->pivot = glm::vec2(0.5f);

                    if (gameOver)
                        text->text = "PLAYER " + std::to_string(winnerId + 1) + " WINS";
                    else if (!watchedName.empty())
                        text->text = "SPECTATING " + watchedName;
                    else
                        text->text = "YOU DIED";
                }
            }

            meshC->enabled = ship->isAlive;
        }
    }
};
