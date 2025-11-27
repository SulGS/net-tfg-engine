#ifndef IECSGAMERENDERER_HPP
#define IECSGAMERENDERER_HPP

#include "netcode/netcode_common.hpp"
#include "netcode/client_window.hpp"
#include "OpenGLWindow.hpp"
#include "IGameRenderer.hpp"
#include "Mesh.hpp"
#include "RenderSystem.hpp"
#include "ecs/UI/UIButton.hpp"
#include "ecs/UI/UIElement.hpp"
#include "ecs/UI/UIImage.hpp"
#include "ecs/UI/UIText.hpp"
#include "ecs/UI/UIRenderSystem.hpp"

class IECSGameRenderer : public IGameRenderer {
protected:
    ECSWorld world;

public:

    virtual void InitECSRenderer(const GameStateBlob& state, OpenGLWindow* window) = 0;

    virtual void GameState_To_ECSWorld(const GameStateBlob& state) = 0;

    void Init(const GameStateBlob& state, OpenGLWindow* window) override {
        world.GetEntityManager().RegisterComponentType<Transform>();
        world.GetEntityManager().RegisterComponentType<Playable>();
        world.GetEntityManager().RegisterComponentType<MeshComponent>();
        world.GetEntityManager().RegisterComponentType<Camera>();
        
        world.GetEntityManager().RegisterComponentType<UIElement>();
        world.GetEntityManager().RegisterComponentType<UIButton>();
        world.GetEntityManager().RegisterComponentType<UIImage>();
        world.GetEntityManager().RegisterComponentType<UIText>();
        
        world.AddSystem(std::make_unique<DestroyingSystem>());

        InitECSRenderer(state, window);
        
        world.AddSystem(std::make_unique<CameraSystem>());
        world.AddSystem(std::make_unique<RenderSystem>());
        world.AddSystem(std::make_unique<UIRenderSystem>(window->getWidth(),window->getHeight()));

        UIRenderSystem* uir = world.GetSystem<UIRenderSystem>();

        uir->LoadFont("default", "C:/Windows/Fonts/arial.ttf", 32);
    }

    void Render(const GameStateBlob& state, OpenGLWindow* window) override {
        GameState_To_ECSWorld(state);

        UIRenderSystem* ui_system = world.GetSystem<UIRenderSystem>();
        ui_system->UpdateScreenSize(window->getWidth(),window->getHeight());
        
        world.Update(false, 1.0f / RENDER_TICKS_PER_SECOND); // Assume 60 FPS for now
    }

};

#endif /* IGameRenderer_hpp */