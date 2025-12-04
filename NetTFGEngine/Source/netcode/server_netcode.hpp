#ifndef SERVER_NETCODE_H
#define SERVER_NETCODE_H
#include "netcode_common.hpp"
#include <set>
#include <algorithm>
#include <cmath>

// Server-side rollback netcode
class ServerNetcode {
public:
    ServerNetcode(std::unique_ptr<IGameLogic> logic) {
        SetGameLogic(std::move(logic));
    }

    void OnClientInputReceived(InputEntry input) {
        std::lock_guard<std::mutex> lk(mtx);
        appliedInputs[input.frame][input.playerId] = input;
    }

    void OnPlayerConnected(int playerId) {
        std::lock_guard<std::mutex> lk(mtx);
        connectedPlayers.insert(playerId);
    }

    void OnPlayerReconnected(int playerId) {
        std::lock_guard<std::mutex> lk(mtx);
        connectedPlayers.insert(playerId);
    }

    void OnPlayerDisconnected(int playerId) {
        std::lock_guard<std::mutex> lk(mtx);
        connectedPlayers.erase(playerId);
    }

    InputEntry GetInputForPlayerAtFrame(int playerId, int frame) {
        std::lock_guard<std::mutex> lk(mtx);
        auto frameIt = appliedInputs.find(frame);
        if (frameIt == appliedInputs.end()) {
            return InputEntry{ frame, MakeZeroInputBlob(), playerId };
        }

        auto playerIt = frameIt->second.find(playerId);
        return playerIt != frameIt->second.end() ?
            playerIt->second :
            InputEntry{ frame, MakeZeroInputBlob(), playerId };
    }

    int GetSizeOfInputsAtFrame(int frame) {
        std::lock_guard<std::mutex> lk(mtx);
        auto frameIt = appliedInputs.find(frame);
        if (frameIt == appliedInputs.end()) return 0;
        return frameIt->second.size();
    }

   

    // ✅ FIXED: Now thread-safe with mutex lock
    StateUpdate Tick() {
        std::lock_guard<std::mutex> lk(mtx);

        SimulateFrame(currentFrame);

        currentFrame++;

        // Create state update
        StateUpdate update;
        update.frame = currentFrame;
        update.state = gameState;

        

        // Cleanup old frames every 60 frames
        if (currentFrame % 60 == 0) {
            CleanupOldFramesInternal();
        }

        return update;
    }

    int GetCurrentFrame() {
        std::lock_guard<std::mutex> lk(mtx);
        return currentFrame;
    }

    GameStateBlob GetCurrentState() {
        std::lock_guard<std::mutex> lk(mtx);
        return gameState;
    }

    void SetGameLogic(std::unique_ptr<IGameLogic> logic) {
        std::lock_guard<std::mutex> lk(mtx);
        gameLogic = std::move(logic);
        gameLogic->isServer = true;
        gameLogic->Init(gameState);
        gameState.frame = 0;
    }

    // ✅ FIXED: Now thread-safe with mutex lock
    IGameLogic* GetGameLogic() {
        std::lock_guard<std::mutex> lk(mtx);
        return gameLogic.get();
    }

    

private:
    std::mutex mtx;
    int currentFrame = 0;
    GameStateBlob gameState;
    std::unique_ptr<IGameLogic> gameLogic;
    InputHistory appliedInputs;
    EventsHistory appliedEvents;
    std::set<int> connectedPlayers;

    // ✅ FIXED: Now private and assumes lock is held
    void SimulateFrame(int frame) {
        // Assumes caller holds mtx lock

        std::map<int, InputEntry> inputs;
        auto frameInIt = appliedInputs.find(frame);
        if (frameInIt != appliedInputs.end()) {
            inputs = frameInIt->second;
        }

        /*for (auto& entry : inputs) {
			// Print inputs for debugging
			std::cout << "Frame " << frame << " - Player " << entry.first << " Input: ";

            for (int i = 0; i < 4; i++) 
            {
                std::cout << static_cast<int>(entry.second.input.data[i]);
            }

            std::cout << "\n";
		}*/

        std::vector<EventEntry> events;
        auto frameEvIt = appliedEvents.find(frame);
        if (frameEvIt != appliedEvents.end()) {
            events = frameEvIt->second;
        }

        gameLogic->SimulateFrame(gameState, events, inputs);

		//gameLogic->PrintState(gameState);

        for (auto& event : gameLogic->generatedEvents) {
            event.frame = frame + 1;
        }
        appliedEvents[frame + 1] = gameLogic->generatedEvents;
        gameState.frame = frame+1;
    }

    // ✅ NEW: Cleanup old frames to prevent unbounded memory growth
    // Internal version called from Tick() - assumes lock is held
    void CleanupOldFramesInternal() {
        // Keep last 300 frames (5 seconds at 60fps)
        const int FRAMES_TO_KEEP = 300;
        int minFrameToKeep = currentFrame - FRAMES_TO_KEEP;

        // Clean up old inputs
        for (auto it = appliedInputs.begin(); it != appliedInputs.end(); ) {
            if (it->first < minFrameToKeep) {
                it = appliedInputs.erase(it);
            }
            else {
                ++it;
            }
        }

        // Clean up old events
        for (auto it = appliedEvents.begin(); it != appliedEvents.end(); ) {
            if (it->first < minFrameToKeep) {
                it = appliedEvents.erase(it);
            }
            else {
                ++it;
            }
        }
    }
};

#endif // SERVER_NETCODE_H