#pragma once

#include "ecs/ecs_common.hpp"
#include "ecs/UI/WorldText.hpp"
#include "../Components.hpp"
#include <string>

// Name of the player above every ship but our own: a WorldText on the ship entity (added in InitECSRenderer), filled
// from Playable::name, or "Player N" while it's unknown (it arrives from the server after the ships exist; offline
// there is none). Hidden on the local ship and on dead ships.
class PlayerNameTagSystem : public ISystem
{
public:
    void Update(EntityManager& entityManager, std::vector<EventEntry>& events, bool isServer, float deltaTime) override
    {
        auto query = entityManager.CreateQuery<Playable, SpaceShip, WorldText>();
        for (auto [entity, play, ship, tag] : query)
        {
            tag->visible = !play->isLocal && ship->isAlive;
            if (!tag->visible) continue;

            // Until the server's names arrive (and offline) the slot number, numbered from 1 like the spectator text.
            // Only on the label: Playable::name stays empty, so "no name yet" can still be told apart.
            if (play->name.empty())
            {
                const std::string fallback = "Player " + std::to_string(play->playerId + 1);
                if (tag->text != fallback)
                    tag->text = fallback;
            }
            else if (tag->text != play->name)
            {
                tag->text = play->name;
            }
        }
    }
};
