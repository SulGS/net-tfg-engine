#ifndef IECSGAMERENDERER_HPP
#define IECSGAMERENDERER_HPP

#include "netcode/netcode_common.hpp"
#include "netcode/client_window.hpp"
#include "ecs/ecs_gamelogic.hpp"
#include "OpenGLWindow.hpp"
#include "GLStateReset.hpp"
#include "GPUMemoryLog.hpp"
#include "IGameRenderer.hpp"
#include "Mesh.hpp"
#include "OpenGL/Render pipeline/RenderSystem.hpp"
#include "OpenGL/Particles/ParticleSystem.hpp"
#include "OpenGL/Particles/ParticleEmitterComponent.hpp"
#include "OpenGL/Particles/ParticlePresets.hpp"
#include "ecs/UI/UIButton.hpp"
#include "ecs/UI/UIElement.hpp"
#include "ecs/UI/UIImage.hpp"
#include "ecs/UI/UIText.hpp"
#include "ecs/UI/UITextField.hpp"
#include "ecs/UI/UIRenderSystem.hpp"
#include "ecs/UI/UIUpdateSystem.hpp"
#include "ecs/UI/UIScrollView.hpp"
#include "ecs/UI/DebugOverlay.hpp"
#include "OpenAL/AudioManager.hpp"
#include "Utils/Input.hpp"

#include <algorithm>
#include <chrono>
#include <functional>


class IECSGameRenderer : public IGameRenderer {
protected:
    ECSWorld world;

    int frameCount = 0;

    // Render-side clock: world.Update() gets the real time since the previous Render(), so every render system
    // (particles, fluid uTime, wall/laser animation, camera shake, UI) runs in real time even when the target FPS
    // isn't reached. Capped so a hitch, a minimised window or re-activating this scene doesn't jump ahead.
    static constexpr float kMaxRenderDt = 0.1f;
    std::chrono::steady_clock::time_point lastRenderTime;
    bool hasLastRenderTime = false;

    float NextRenderDeltaTime() {
        const auto now = std::chrono::steady_clock::now();
        float dt = 1.0f / CurrentTargetFPS(); // first frame: nothing to measure yet
        if (hasLastRenderTime)
            dt = std::chrono::duration<float>(now - lastRenderTime).count();
        lastRenderTime = now;
        hasLastRenderTime = true;
        return std::clamp(dt, 0.0f, kMaxRenderDt);
    }

    std::function<void(IECSGameLogic* logic, IECSGameRenderer* renderer)> renderDataTransferToLogicCallback;

public:

    virtual void InitECSRenderer(const GameStateBlob& state, OpenGLWindow* window) = 0;

    virtual void GameState_To_ECSWorld(const GameStateBlob& state) = 0;

    // Public access to this renderer's EntityManager. AudioSourceComponent,
    // UIImage, MeshComponent etc. all live here (registered in Init() below),
    // not in the logic world — see Client::GetRendererEntityManager().
    EntityManager& GetEntityManager() { return world.GetEntityManager(); }

    void Init(const GameStateBlob& state, OpenGLWindow* window) override {
        world.Reset();
        hasLastRenderTime = false;

        world.GetEntityManager().RegisterComponentType<Transform>();
        world.GetEntityManager().RegisterComponentType<Playable>();
        world.GetEntityManager().RegisterComponentType<MeshComponent>();
        world.GetEntityManager().RegisterComponentType<Camera>();

        world.GetEntityManager().RegisterComponentType<UIElement>();
        world.GetEntityManager().RegisterComponentType<UIButton>();
        world.GetEntityManager().RegisterComponentType<UIImage>();
        world.GetEntityManager().RegisterComponentType<UIText>();
        world.GetEntityManager().RegisterComponentType<UITextField>();
		world.GetEntityManager().RegisterComponentType<UISlider>();
		world.GetEntityManager().RegisterComponentType<UIDropdown>();
		UIScroll::Register(world.GetEntityManager());

        world.GetEntityManager().RegisterComponentType<DirectionalLightComponent>();
        world.GetEntityManager().RegisterComponentType<PointLightComponent>();

        world.GetEntityManager().RegisterComponentType<AudioSourceComponent>();
        world.GetEntityManager().RegisterComponentType<AudioListenerComponent>();

        world.GetEntityManager().RegisterComponentType<ParticleEmitterComponent>();

        DebugOverlay::Register(world.GetEntityManager());

        world.AddSystem(std::make_unique<DestroyingSystem>());

        InitECSRenderer(state, window);

        // Built after InitECSRenderer so it draws over whatever the scene added.
        DebugOverlay::Build(world.GetEntityManager());
        world.AddSystem(std::make_unique<DebugOverlaySystem>());

        world.AddSystem(std::make_unique<CameraSystem>());
        world.AddSystem(std::make_unique<ParticleSystem>());
        world.AddSystem(std::make_unique<RenderSystem>());
        world.AddSystem(std::make_unique<UIRenderSystem>(1920,1080));

        AudioManager::SetEntityManager(&world.GetEntityManager());

        ParticleSystem* particleSys = world.GetSystem<ParticleSystem>();
        particleSys->Init();

        RenderSystem* renderSys = world.GetSystem<RenderSystem>();
        int renderW, renderH;
        RenderSettings::instance().computeRenderSize(window->getWidth(), window->getHeight(), renderW, renderH);
        renderSys->Init(renderW, renderH, window->getWidth(), window->getHeight());
        renderSys->SetParticleSystem(particleSys);


        UIRenderSystem* uir = world.GetSystem<UIRenderSystem>();

        world.AddSystem(std::make_unique<UIUpdateSystem>(1920, 1080, window->getWindow(), uir->GetFontManager()));

        uir->LoadFont("default", "C:/Windows/Fonts/arial.ttf", 32);

        LogGPUMemory("World set up");
    }

    // Destroys every component in this world, dropping AssetManager ref-counts and GL resources; must run on the render thread and takes the same EntityManager mutex as Render().
    void ReleaseECSAssets() override {
        world.GetEntityManager().acquireMutex();
        // Nothing of this world may stay bound while its textures/buffers/programs are deleted (see GLStateReset.hpp).
        ResetGLBindings();
        LogGPUMemory("World teardown, before");
        // GPU idle before and after the mass delete: no queued frame still references what is deleted, and the driver
        // has retired the deletions before the next world draws. The Radeon 520 (driver 21.19) crashed inside the
        // driver in the next world's first draw after a match was torn down (use-after-free-looking write address).
        glFinish();
        world.Reset();
        glFinish();
        LogGPUMemory("World teardown, after");
        LogGLErrors("World teardown");
        world.GetEntityManager().releaseMutex();
    }

    void Render(const GameStateBlob& state, OpenGLWindow* window) override {
        // Must hold the EntityManager mutex: the independent audio thread takes it too, and without this lock DestroyingSystem can race it and corrupt memory.
        EntityManager& em = world.GetEntityManager();
        em.acquireMutex();

        GameState_To_ECSWorld(state);

        RenderSystem* renderSys = world.GetSystem<RenderSystem>();

        // F12: dump this frame's image after every render pass plus all render targets to Render/<timestamp>/.
        if (Input::KeyTapped(GLFW_KEY_F12))
            renderSys->RequestDebugDump();
        // F11: only the final frame, without the HUD/UI, to Render/<timestamp>.png.
        if (Input::KeyTapped(GLFW_KEY_F11))
            renderSys->RequestFinalFrameDump();

        // Checked every frame rather than on wasResized(): the render resolution setting can change without the
        // window doing so. Resize() is a no-op when nothing changed. The flag is still consumed so it doesn't linger.
        window->wasResized();
        {
            const int outW = window->getWidth();
            const int outH = window->getHeight();
            int renderW, renderH;
            RenderSettings::instance().computeRenderSize(outW, outH, renderW, renderH);
            if (renderW != renderSys->GetScreenWidth() || renderH != renderSys->GetScreenHeight()
                || outW != renderSys->GetOutputWidth() || outH != renderSys->GetOutputHeight())
                renderSys->Resize(renderW, renderH, outW, outH);
        }

        auto activeCamera = em.CreateQuery<Camera, Transform>();

        for (auto [entity, camera, transform] : activeCamera) {
            if (window->getHeight() > 0)
                camera->updateAspectRatio(static_cast<float>(window->getWidth()) / window->getHeight());
            break; // Only one camera supported for now
        }

        UIRenderSystem* ui_system = world.GetSystem<UIRenderSystem>();
        ui_system->UpdateScreenSize(window->getLogicalWidth(), window->getLogicalHeight());

        UIUpdateSystem* ui_update = world.GetSystem<UIUpdateSystem>();
        ui_update->UpdateScreenSize(window->getLogicalWidth(), window->getLogicalHeight());

        auto t1 = std::chrono::high_resolution_clock::now();
        // Minimized: keep game/audio systems advancing, skip only GL drawing.
        world.Update(false, NextRenderDeltaTime(), window->isMinimized());

		for (auto& system : world.GetSystems()) {
			if (system.get()->requestRenderReinit) {
				system.get()->requestRenderReinit = false;
				renderSys->needsReinit = true;
			}
		}

        em.releaseMutex();

        auto t2 = std::chrono::high_resolution_clock::now();

        if (renderDataTransferToLogicCallback) {
            IECSGameLogic* logic = static_cast<IECSGameLogic*>(GetGameLogic());
            renderDataTransferToLogicCallback(logic, this);
        }

        frameCount++;

        if (frameCount >= CurrentTargetFPS() * 25) {
            frameCount = 0;
        }
    }

};

#endif /* IGameRenderer_hpp */