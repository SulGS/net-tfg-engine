#pragma once

#include "ecs/ecs_common.hpp"
#include "../Components.hpp"

// Returns the playerId of the sole surviving player, or -1 if the game is
// still ongoing (0 or 2+ players alive).
inline int GetWinnerId(EntityManager& entityManager)
{
    int aliveCount = 0;
    int winnerId = -1;
    int total = 0;

    auto q = entityManager.CreateQuery<Playable, SpaceShip>();
    for (auto [e, play, ship] : q)
    {
        total++;
        if (ship->isAlive) { aliveCount++; winnerId = play->playerId; }
    }

    return (total >= 2 && aliveCount == 1) ? winnerId : -1;
}
