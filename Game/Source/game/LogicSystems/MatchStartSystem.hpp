#pragma once

#include "ecs/ecs.hpp"
#include "../Components.hpp"

// Server-authoritative pre-match countdown: the only place that decrements the MatchStartTimer entity.
// InputSystem and ArenaSystem gate on the synced result (see InputSystem::Update).
class MatchStartSystem : public ISystem {
public:
    void Update(EntityManager& entityManager, std::vector<EventEntry>& events, bool isServer, float deltaTime) override {
        if (!isServer) return;

        auto query = entityManager.CreateQuery<MatchStartTimer>();
        for (auto [entity, timer] : query)
            if (timer->ticksRemaining > 0) timer->ticksRemaining--;
    }
};
