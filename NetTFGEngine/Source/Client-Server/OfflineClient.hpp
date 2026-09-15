#pragma once

#include "netcode/netcode_common.hpp"
#include "netcode/client_window.hpp"
#include "OpenGL/IGameRenderer.hpp"
#include "OpenGL/IECSGameRenderer.hpp"
#include <memory>
#include <chrono>
#include <thread>
#include <cstdint>
#include "Utils/Debug/Debug.hpp"
#include "Client-Server/Client.hpp"

class OfflineClient : public Client {
public:
    OfflineClient(std::unique_ptr<IGameLogic> gameLogic,
        std::unique_ptr<IGameRenderer> gameRenderer, std::string binFileName)
        : gameLogic_(std::move(gameLogic))
        , gameRenderer_(std::move(gameRenderer))
        , assignedPlayerId_(0)
        , cWindow_(nullptr)
    {
        binName = binFileName;
    }

    ConnectionCode SetupClient(const std::string& hostStr = "0.0.0.0", uint16_t port = 0, const std::string& customClientId = "") override {
        Debug::Info("OfflineClient") << "Starting offline game (ignoring host and port parameters)\n";

        // Guard against re-activation while already set up; routed through
        // RunOnRenderThread so deleting cWindow_ can't race renderLoop()'s
        // activeInstances snapshot (past cause of a menu->settings->menu use-after-free).
        if (cWindow_) {
            ClientWindow::RunOnRenderThread([this]() {
                cWindow_->deactivate();
                delete cWindow_;
                cWindow_ = nullptr;
                });
        }

        isOfflineClient = true;

        gameRenderer_->playerId = assignedPlayerId_;
        gameLogic_->playerId = assignedPlayerId_;

        cWindow_ = new ClientWindow(
            [this](GameStateBlob& state, OpenGLWindow* win) {
                gameRenderer_->Init(state, win);
            },
            [this](GameStateBlob& state, OpenGLWindow* win) {
                gameRenderer_->Render(state, win);
            },
            [this](const GameStateBlob& previousServerState, const GameStateBlob& currentServerState, const GameStateBlob& previousLocalState, const GameStateBlob& currentLocalState, GameStateBlob& renderState, float serverInterpolation, float localInterpolation) {
                gameRenderer_->Interpolate(previousServerState, currentServerState, previousLocalState, currentLocalState, renderState, serverInterpolation, localInterpolation);
            }
        );

        gameLogic_->Init(gameState_);

        cWindow_->activate();

        Debug::Info("OfflineClient") << "Offline client setup OK\n";

        return CONN_SUCCESS;
    }

    void TickClient() override {
        InputBlob localInput = gameLogic_->GenerateLocalInput();

        std::vector<EventEntry> events;
        std::map<int, InputEntry> inputs;

        InputEntry entry = { currentFrame_,localInput,0 };


        inputs[0] = entry;

        gameLogic_->SimulateFrame(gameState_, events, inputs);

        cWindow_->setLocalState(gameState_);
        cWindow_->setServerState(gameState_);

        if (currentFrame_ % 30 == 0) {
            Debug::Info("OfflineClient") << "[OFFLINE] Frame: " << currentFrame_ << "\n";
        }

        currentFrame_++;
    }

    void CloseClient() override {
        // Runs as ONE task on the render thread so deactivate/delete can't race
        // renderLoop(), and must happen before ReleaseECSAssets() so Render()
        // never sees a torn-down world for a still-active instance.
        ClientWindow::RunOnRenderThread([this]() {
            if (cWindow_) {
                cWindow_->deactivate();
                delete cWindow_;
                cWindow_ = nullptr;  // prevent dangling pointer on re-activation
            }

            // Release ECS-held AssetManager refs before the caller unloads this client's asset bin.
            if (gameRenderer_) gameRenderer_->ReleaseECSAssets();
            if (gameLogic_)    gameLogic_->ReleaseECSAssets();
            });

        currentFrame_ = 0;      // reset so the next session starts from frame 0
        Debug::Info("OfflineClient") << "[OFFLINE] Offline client finished\n";
    }

	EntityManager* GetEntityManager() override {
		IECSGameLogic* ecsLogic = dynamic_cast<IECSGameLogic*>(gameLogic_.get());
		if (ecsLogic) {
			return &ecsLogic->world.GetEntityManager();
		}
        return nullptr;
	}

	EntityManager* GetRendererEntityManager() override {
		IECSGameRenderer* ecsRenderer = dynamic_cast<IECSGameRenderer*>(gameRenderer_.get());
		if (ecsRenderer) {
			return &ecsRenderer->GetEntityManager();
		}
		return nullptr;
	}

private:
    std::unique_ptr<IGameLogic> gameLogic_;
    std::unique_ptr<IGameRenderer> gameRenderer_;
    int assignedPlayerId_;

    ClientWindow* cWindow_;
    GameStateBlob gameState_;
    int currentFrame_ = 0;


};