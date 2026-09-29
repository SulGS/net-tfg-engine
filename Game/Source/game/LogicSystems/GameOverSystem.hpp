#pragma once

#include "ecs/ecs_common.hpp"
#include "../Components.hpp"

class GameOverSystem : public ISystem {
    float gameOverTimer = -1.0f; // -1 = not started
    bool  timerFired = false;
public:
    void Update(EntityManager& entityManager, std::vector<EventEntry>& events, bool isServer, float deltaTime) override {
        if (!isServer) return;
        if (timerFired)  return;

        int  aliveCount = 0;
        int  winnerId = -1;
        int  totalPlayers = 0;

        auto query = entityManager.CreateQuery<Playable, SpaceShip>();
        for (auto [entity, play, ship] : query)
        {
            totalPlayers++;
            if (ship->isAlive)
            {
                aliveCount++;
                winnerId = play->playerId;
            }
        }

        // Need at least 2 players to have started, and exactly 1 left alive
        if (totalPlayers < 2 || aliveCount != 1)
        {
            // If a new game-over condition wasn't met, reset the timer
            if (aliveCount != 1) gameOverTimer = -1.0f;
            return;
        }

        if (gameOverTimer < 0.0f)
            gameOverTimer = 10.0f;

        gameOverTimer -= deltaTime;

        if (gameOverTimer <= 0.0f)
        {
            timerFired = true;

            emitGameFinishEvent = true;
        }
    }
};
