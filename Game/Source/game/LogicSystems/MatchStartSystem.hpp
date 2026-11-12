#pragma once

#include "ecs/ecs.hpp"
#include "../Components.hpp"

// Server-authoritative pre-match countdown: the only place that decrements the MatchStartTimer entity.
// InputSystem and ArenaSystem gate on the synced result (see InputSystem::Update).
class MatchStartSystem : public ISystem {
public:
    void Update(EntityManager& entityManager, std::vector<EventEntry>& events, bool isServer, float deltaTime) override {
        // Client and server alike: a plain integer countdown predicts exactly. Run only on the server, the client's
        // prediction kept the old value and every one of the countdown's frames reconciled (alpha test 2026-11-11).
        auto query = entityManager.CreateQuery<MatchStartTimer>();
        for (auto [entity, timer] : query)
            if (!timer->waitingForPlayers && timer->ticksRemaining > 0) timer->ticksRemaining--;
    }
};
