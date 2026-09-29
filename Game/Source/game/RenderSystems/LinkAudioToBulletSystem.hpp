#pragma once

#include "ecs/ecs_common.hpp"
#include "OpenAL/AudioComponents.hpp"
#include "../Components.hpp"

class LinkAudioToBulletSystem : public ISystem
{
public:
	void Update(
		EntityManager& entityManager,
		std::vector<EventEntry>& events,
		bool isServer,
		float deltaTime
	) override
	{
		auto audioQuery = entityManager.CreateQuery<Transform, AudioSourceComponent, LinkAudioToBullet>();
		auto bulletQuery = entityManager.CreateQuery<Transform, ECSBullet>();
		for (auto [audioEntity, audioTransform, audio, link] : audioQuery)
		{
			for (auto [bulletEntity, bulletTransform, bullet] : bulletQuery)
			{
				if (bullet->id == link->bulletId)
				{
					audioTransform->setPosition(bulletTransform->getPosition());
					break;
				}
			}
		}
	}
};
