#pragma once

#include <algorithm>
#include <glm/glm.hpp>

#include "ecs/ecs_common.hpp"
#include "OpenAL/AudioComponents.hpp"
#include "../Components.hpp"

class ThrustersSoundSystem : public ISystem
{
public:
	void Update(
		EntityManager& entityManager,
		std::vector<EventEntry>& events,
		bool isServer,
		float deltaTime
	) override
	{
		auto audioQuery = entityManager.CreateQuery<Transform, AudioSourceComponent, ThrusterSound>();
		auto shipQuery = entityManager.CreateQuery<Transform, Playable, SpaceShip>();
		for (auto [audioEntity, soundT, audio, thrusterOwner] : audioQuery)
		{
			for (auto [shipEntity, shipT, play, ship] : shipQuery)
			{
				if (thrusterOwner->shipEntity != play->playerId) continue;
				if (!ship->isAlive)
				{
					// `play` only ever triggers alSourcePlay (see AudioSystem::Update) — it
					// never stops an already-looping source, so the engine loop would keep
					// looping forever at its last pitch/gain if we didn't silence it here.
					audio->play = false;
					audio->gain = 0.0f;
				}
				else
				{
					audio->play = true;

                    soundT->setPosition(shipT->getPosition());

					float speed = glm::length(glm::vec2(ship->velX, ship->velY));
                    float t = std::min(speed / 3.0f, 1.0f);

                    audio->gain = 0.05f + 0.45f * t;   // 0.05 idle → 0.50 full
                    audio->pitch = 0.5f + 0.7f * t;   // 0.5 idle → 1.2 full
				}
			}
		}
	}
};
