#pragma once

#include <algorithm>
#include <string>

#include "ecs/ecs_common.hpp"
#include "ecs/UI/UIElement.hpp"
#include "ecs/UI/UIImage.hpp"
#include "ecs/UI/UIText.hpp"
#include "Utils/Input.hpp"
#include "Utils/InputMap.hpp"
#include "../Components.hpp"
#include "../GameActions.hpp"

// HUD for the local player's two abilities, bottom centre: one icon each, with
//   ready     -> icon at full brightness (and a short pulse the moment it becomes ready),
//   active    -> icon tinted in the shield's cyan,
//   cooldown  -> icon dimmed, a bright copy of it refilling from the bottom, and the seconds left on top,
// and the bound key/button under it (whichever device was used last). Hidden while dead/spectating.
namespace AbilityHud {
    inline constexpr float ICON_SIZE = 84.0f;
    inline constexpr float SPACING = 120.0f;     // centre to centre
    inline constexpr float BOTTOM_MARGIN = 44.0f; // icon bottom above the screen bottom (the key label goes below)
    inline constexpr int   LAYER = 5;             // over the game status text, under the pause menu (50)

    inline const char* const ICONS[2] = { "icono-propulsion.png", "icono-escudo-laser.png" };
    inline const int ACTIONS[2] = { GameAction::Dash, GameAction::Shield };

    inline float SlotX(int ability) { return (ability == 0 ? -0.5f : 0.5f) * SPACING; }

    inline UIElement* AddElement(EntityManager& em, Entity e, int ability, int layer)
    {
        UIElement* el = em.AddComponent<UIElement>(e, UIElement{});
        el->anchor = UIAnchor::BOTTOM_CENTER;
        el->position = glm::vec2(SlotX(ability), -BOTTOM_MARGIN);
        el->size = glm::vec2(ICON_SIZE);
        el->pivot = glm::vec2(0.5f, 1.0f);   // bottom-centre: the refill grows upwards from the same edge
        el->layer = layer;
        return el;
    }

    inline void Build(EntityManager& em)
    {
        for (int a = 0; a < 2; a++)
        {
            Entity base = em.CreateEntity();
            AddElement(em, base, a, LAYER);
            em.AddComponent<UIImage>(base, UIImage{})->texturePath = ICONS[a];
            em.AddComponent<AbilityHudIcon>(base, AbilityHudIcon{ a, 0 });

            Entity fill = em.CreateEntity();
            AddElement(em, fill, a, LAYER + 1);
            em.AddComponent<UIImage>(fill, UIImage{})->texturePath = ICONS[a];
            em.AddComponent<AbilityHudIcon>(fill, AbilityHudIcon{ a, 1 });

            Entity key = em.CreateEntity();
            UIElement* keyEl = AddElement(em, key, a, LAYER);
            keyEl->position = glm::vec2(SlotX(a), -10.0f);
            keyEl->size = glm::vec2(SPACING, 28.0f);
            UIText* keyText = em.AddComponent<UIText>(key, UIText{});
            keyText->fontSize = 20.0f;
            keyText->align = UITextAlign::CENTER;
            keyText->SetColor(0.85f, 0.88f, 0.92f, 0.9f);
            keyText->SetFont("default");
            em.AddComponent<AbilityHudIcon>(key, AbilityHudIcon{ a, 2 });

            Entity secs = em.CreateEntity();
            AddElement(em, secs, a, LAYER + 2);
            UIText* secsText = em.AddComponent<UIText>(secs, UIText{});
            secsText->fontSize = 34.0f;
            secsText->align = UITextAlign::CENTER;
            secsText->SetColor(1.0f, 1.0f, 1.0f, 1.0f);
            secsText->SetFont("default");
            em.AddComponent<AbilityHudIcon>(secs, AbilityHudIcon{ a, 3 });
        }
    }
}

class AbilityHudSystem : public ISystem
{
    const float PULSE_SECONDS = 0.35f;

public:
    void Update(EntityManager& entityManager, std::vector<EventEntry>& events, bool isServer, float deltaTime) override
    {
        const SpaceShip* local = nullptr;
        {
            auto shipQuery = entityManager.CreateQuery<Playable, SpaceShip>();
            for (auto [entity, play, ship] : shipQuery)
                if (play->isLocal) local = ship;
        }
        const bool visible = local && local->isAlive;

        const bool pad = Input::LastDevice() == Input::Device::Gamepad;

        auto hudQuery = entityManager.CreateQuery<UIElement, AbilityHudIcon>();
        for (auto [entity, el, icon] : hudQuery)
        {
            el->isVisible = visible;
            if (!visible) continue;

            const int a = icon->ability;
            const int activeTicks = (a == 0) ? local->dashTicks : local->shieldTicks;
            const int cooldown = (a == 0) ? local->dashCooldown : local->shieldCooldown;
            const int cooldownTotal = (a == 0) ? DASH_COOLDOWN_TICKS : SHIELD_COOLDOWN_TICKS;
            const bool active = activeTicks > 0;
            const bool ready = !active && cooldown <= 0;

            // Refilled fraction: 0 right after use, 1 when ready again.
            const float refill = ready ? 1.0f
                : active ? 0.0f
                : 1.0f - std::clamp(static_cast<float>(cooldown) / cooldownTotal, 0.0f, 1.0f);

            // Pulse when the cooldown finishes (only the base icon uses it; every part tracks it the same way).
            if (ready && !icon->wasReady) icon->readyPulse = 1.0f;
            icon->wasReady = ready;
            icon->readyPulse = std::max(icon->readyPulse - deltaTime / PULSE_SECONDS, 0.0f);
            const float scale = 1.0f + 0.18f * icon->readyPulse * icon->readyPulse;

            switch (icon->part)
            {
            case 0: // base: full icon, dimmed unless ready
            {
                el->size = glm::vec2(AbilityHud::ICON_SIZE * scale);
                if (UIImage* img = entityManager.GetComponent<UIImage>(entity))
                {
                    if (ready)       img->SetColor(1.0f, 1.0f, 1.0f, 1.0f);
                    else if (active) img->SetColor(0.55f, 0.85f, 1.0f, 1.0f);
                    else             img->SetColor(0.28f, 0.29f, 0.32f, 0.85f);
                }
                break;
            }
            case 1: // refill: the bottom `refill` of the icon at full brightness, only while cooling down
            {
                const bool show = !ready && !active && refill > 0.0f;
                el->isVisible = show;
                el->size = glm::vec2(AbilityHud::ICON_SIZE, AbilityHud::ICON_SIZE * refill);
                if (UIImage* img = entityManager.GetComponent<UIImage>(entity))
                {
                    img->SetUVRect(0.0f, 0.0f, 1.0f, refill); // v = 0 is the image's bottom (textures load Y-flipped)
                    img->SetColor(0.75f, 0.78f, 0.82f, 0.9f);
                }
                break;
            }
            case 2: // bound key/button
            {
                if (UIText* text = entityManager.GetComponent<UIText>(entity))
                {
                    const InputBinding binding = InputMap::Get().GetBinding(AbilityHud::ACTIONS[a],
                        pad ? BindingSlot::Gamepad : BindingSlot::Keyboard);
                    text->text = binding.DisplayName();
                }
                break;
            }
            case 3: // seconds left on the cooldown
            {
                if (UIText* text = entityManager.GetComponent<UIText>(entity))
                {
                    const bool show = !ready && !active;
                    el->isVisible = show;
                    if (show) text->text = std::to_string((cooldown + TICKS_PER_SECOND - 1) / TICKS_PER_SECOND);
                }
                break;
            }
            }
        }
    }
};
