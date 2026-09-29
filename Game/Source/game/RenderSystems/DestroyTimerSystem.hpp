#pragma once

#include "ecs/ecs.hpp"
#include "OpenAL/AudioComponents.hpp"
#include "../Components.hpp"

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
            destroyTimer->secondsRemaining -= deltaTime;

            if (destroyTimer->secondsRemaining <= 0.0f)
            {

                audio->pendingToDestroy = true;
            }
        }
    }
};
