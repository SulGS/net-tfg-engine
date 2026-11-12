#pragma once

#include <string>

#include "netcode/netcode_common.hpp"
#include "ecs/ecs.hpp"
#include "ecs/UI/UIElement.hpp"
#include "ecs/UI/UIText.hpp"
#include "../Components.hpp"

// Overrides the GameStatusText label with a centred countdown during the pre-match freeze (MatchStartTimer).
// Must run AFTER CameraFollowSystem to win the frame's last write; does nothing once the countdown reaches 0.
class MatchStartCountdownRenderSystem : public ISystem
{
public:
    void Update(EntityManager& entityManager, std::vector<EventEntry>& events,
        bool isServer, float deltaTime) override
    {
        int ticksRemaining = 0;
        bool waitingForPlayers = false;
        {
            auto timerQuery = entityManager.CreateQuery<MatchStartTimer>();
            for (auto [entity, timer] : timerQuery) {
                ticksRemaining = timer->ticksRemaining;
                waitingForPlayers = timer->waitingForPlayers;
            }
        }
        if (ticksRemaining <= 0) return;

        int secondsRemaining = (ticksRemaining + TICKS_PER_SECOND - 1) / TICKS_PER_SECOND;
        const std::string label = waitingForPlayers ? std::string("Esperando a los demás jugadores...")
                                                    : std::to_string(secondsRemaining);

        auto textQuery = entityManager.CreateQuery<UIElement, UIText, GameStatusText>();
        for (auto [entity, element, text, statusTag] : textQuery)
        {
            element->anchor = UIAnchor::TOP_CENTER;
            element->position = glm::vec2(0.0f, 20.0f);
            element->size = glm::vec2(waitingForPlayers ? 900.0f : 350.0f, 80.0f);
            element->pivot = glm::vec2(0.5f);
            text->text = label;
        }
    }
};
