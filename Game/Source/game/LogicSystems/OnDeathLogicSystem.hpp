#pragma once

#include "ecs/ecs.hpp"

class OnDeathLogicSystem : public ISystem {
public:
    void Update(EntityManager& entityManager, std::vector<EventEntry>& events, bool isServer, float deltaTime) override {
        // Players are permanently dead — no respawn logic needed.
        // Dead state is terminal; isAlive stays false, isSpectating stays true.
    }
};
