#pragma once

// Always-on-top FPS/latency label built by IECSGameRenderer::Init() in every scene. Visibility follows
// RenderSettings::getDebugModeEnabled(), toggled live from the settings menu.

#include "ecs/ecs.hpp"
#include "ecs/UI/UIElement.hpp"
#include "ecs/UI/UIText.hpp"
#include "OpenGL/Render pipeline/RenderSettings.hpp"
#include "netcode/client_window.hpp"
#include "Client-Server/NetworkStats.hpp"

#include <cstdio>

class DebugOverlayTag : public IComponent {};

class DebugOverlaySystem : public ISystem
{
public:
    void Update(EntityManager& entityManager,
        std::vector<EventEntry>& events,
        bool isServer,
        float deltaTime) override
    {
        UIElement* element = nullptr;
        UIText* text = nullptr;

        auto query = entityManager.CreateQuery<UIElement, UIText, DebugOverlayTag>();
        for (auto [entity, el, txt, tag] : query) { element = el; text = txt; }
        if (!element || !text) return;

        const bool enabled = RenderSettings::instance().getDebugModeEnabled();
        element->isVisible = enabled;
        if (!enabled) return;

        char buf[96];
        if (NetworkStats::IsConnected())
        {
            std::snprintf(buf, sizeof(buf), "FPS: %.0f | Latencia: %.0f ms",
                ClientWindow::GetCurrentFPS(), NetworkStats::GetLatencyMs());
        }
        else
        {
            std::snprintf(buf, sizeof(buf), "FPS: %.0f | Latencia: N/A",
                ClientWindow::GetCurrentFPS());
        }
        text->text = buf;
    }
};

namespace DebugOverlay
{
    inline void Register(EntityManager& em)
    {
        em.RegisterComponentType<DebugOverlayTag>();
    }

    // Top-right corner, above every other UI layer in the scene.
    inline Entity Build(EntityManager& em)
    {
        Entity e = em.CreateEntity();

        UIElement* el = em.AddComponent<UIElement>(e, UIElement{});
        el->anchor = UIAnchor::TOP_RIGHT;
        el->position = glm::vec2(-12.0f, 10.0f);
        el->size = glm::vec2(360.0f, 24.0f);
        el->pivot = glm::vec2(1.0f, 0.0f);
        el->isVisible = false;
        el->layer = 10000;

        UIText* txt = em.AddComponent<UIText>(e, UIText{});
        txt->text = "";
        txt->fontSize = 16.0f;
        txt->SetColor(0.15f, 1.0f, 0.35f, 1.0f);
        txt->SetFont("default");

        em.AddComponent<DebugOverlayTag>(e, DebugOverlayTag{});

        return e;
    }
}
