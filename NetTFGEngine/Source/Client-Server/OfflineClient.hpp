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
    {
    }

    int RunClient(const std::string& hostStr = "0.0.0.0", uint16_t port = 0) override {
        Debug::Info("OfflineClient") << "Starting offline game (ignoring host and port parameters)\n";

        gameRenderer_->playerId = assignedPlayerId_;
        gameLogic_->playerId = assignedPlayerId_;

        ClientWindow cWindow(
            [this](GameStateBlob& state, OpenGLWindow* win) {
                gameRenderer_->Init(state, win);
            },
            [this](GameStateBlob& state, OpenGLWindow* win) {
                gameRenderer_->Render(state, win);
            },
            [this](const GameStateBlob& previousServerState, const GameStateBlob& currentServerState,
                const GameStateBlob& previousLocalState, const GameStateBlob& currentLocalState,
                GameStateBlob& renderState, float serverInterpolation, float localInterpolation) {
                    gameRenderer_->Interpolate(previousServerState, currentServerState,
                        previousLocalState, currentLocalState,
                        renderState, serverInterpolation, localInterpolation);
            }
        );

        // Initialize game state
        GameStateBlob currentState;
        gameLogic_->Init(currentState);

        Debug::Info("OfflineClient") << "Running offline game loop\n";
        RunClientLoop(cWindow, currentState);

        return 0;
    }

private:
    std::unique_ptr<IGameLogic> gameLogic_;
    std::unique_ptr<IGameRenderer> gameRenderer_;
    int assignedPlayerId_;

    void RunClientLoop(ClientWindow& cWindow, GameStateBlob& gameState) {
        auto nextTick = std::chrono::high_resolution_clock::now();

        // Start render thread
        std::thread renderThread(&ClientWindow::run, &cWindow);

        int currentFrame = 0;

        while (cWindow.isRunning()) {
            // Generate local input
            InputBlob localInput = gameLogic_->GenerateLocalInput();

            // Apply input to game state
            std::vector<EventEntry> events;
            std::map<int, InputEntry> inputs;

            InputEntry entry = { currentFrame,localInput,0 };


            inputs[0] = entry;

            gameLogic_->SimulateFrame(gameState, events, inputs);

            // Update render states
            cWindow.setLocalState(gameState);
            cWindow.setServerState(gameState);

            // Debug output every 30 frames
            if (currentFrame % 30 == 0) {
                Debug::Info("OfflineClient") << "[OFFLINE] Frame: " << currentFrame << "\n";
            }

            currentFrame++;
            nextTick += std::chrono::milliseconds(MS_PER_TICK);
            std::this_thread::sleep_until(nextTick);
        }

        // Clean shutdown
        renderThread.join();
        Debug::Info("OfflineClient") << "[OFFLINE] Window closed, shutting down\n";
    }
};