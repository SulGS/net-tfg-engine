#pragma once

#include "ecs/ecs_common.hpp"
#include "OpenAL/AudioComponents.hpp"
#include "../Components.hpp"

class UpdateListenerTransformSystem : public ISystem
{
public:
    void Update(
        EntityManager& entityManager,
        std::vector<EventEntry>& events,
        bool isServer,
        float deltaTime
    ) override
    {
        auto listenerQuery = entityManager.CreateQuery<Transform, AudioListenerComponent>();
        auto playerQuery = entityManager.CreateQuery<Transform, Playable, SpaceShip>();
        for (auto [listenerEntity, listenerTransform, listener] : listenerQuery)
        {
            for (auto [playerEntity, playerTransform, play, ship] : playerQuery)
            {
                if (play->isLocal)
                {
                    listenerTransform->setPosition(playerTransform->getPosition());
					listenerTransform->setRotation(playerTransform->getRotation());
                    break;
                }
            }
        }
    }
};
