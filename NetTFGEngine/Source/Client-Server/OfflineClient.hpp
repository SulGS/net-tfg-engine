#pragma once

#include "netcode/netcode_common.hpp"
#include "netcode/client_window.hpp"
#include "OpenGL/IGameRenderer.hpp"
#include <memory>
#include <chrono>
#include <thread>
#include <cstdint>
#include "Utils/Debug/Debug.hpp"
#include "Client-Server/Client.hpp"

class OfflineClient : public Client {
public:
    OfflineClient(std::unique_ptr<IGameLogic> gameLogic,
        std::unique_ptr<IGameRenderer> gameRenderer)
        : gameLogic_(std::move(gameLogic))
        , gameRenderer_(std::move(gameRenderer))
        , assignedPlayerId_(0)
		, cWindow_(nullptr)
    {
    }

    ConnectionCode SetupClient(const std::string& hostStr = "0.0.0.0", uint16_t port = 0, const std::string& customClientId = "") override {
        Debug::Info("OfflineClient") << "Starting offline game (ignoring host and port parameters)\n";

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

        // Initialize game state
        gameLogic_->Init(gameState_);

        cWindow_->activate();

        Debug::Info("OfflineClient") << "Offline client setup OK\n";

        return CONN_SUCCESS;
    }

    void TickClient() override {
        // Generate local input
        InputBlob localInput = gameLogic_->GenerateLocalInput();

        // Apply input to game state
        std::vector<EventEntry> events;
        std::map<int, InputEntry> inputs;

        InputEntry entry = { currentFrame_,localInput,0 };


        inputs[0] = entry;

        gameLogic_->SimulateFrame(gameState_, events, inputs);

        // Update render states
        cWindow_->setLocalState(gameState_);
        cWindow_->setServerState(gameState_);

        // Debug output every 30 frames
        if (currentFrame_ % 30 == 0) {
            Debug::Info("OfflineClient") << "[OFFLINE] Frame: " << currentFrame_ << "\n";
        }

        currentFrame_++;
    }

    void CloseClient() override {

        cWindow_->deactivate();
        delete cWindow_;
        Debug::Info("OfflineClient") << "[OFFLINE] Offline client finalished\n";
    }

private:
    std::unique_ptr<IGameLogic> gameLogic_;
    std::unique_ptr<IGameRenderer> gameRenderer_;
    int assignedPlayerId_;

	ClientWindow* cWindow_;
	GameStateBlob gameState_;
	int currentFrame_ = 0;

    
};