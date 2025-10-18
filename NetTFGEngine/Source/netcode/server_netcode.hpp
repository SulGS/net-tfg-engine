#ifndef SERVER_NETCODE_H
#define SERVER_NETCODE_H

#include "netcode_common.hpp"
#include <set>
#include <algorithm>
#include <cmath>

// Server-side rollback netcode
class ServerRollbackNetcode {
public:
    ServerRollbackNetcode(std::unique_ptr<IGameLogic> logic) {
        SetGameLogic(std::move(logic));
        snapshots.resize(MAX_ROLLBACK_FRAMES);
        SaveSnapshot(0);
    }

    void OnClientInputReceived(int playerId, int frame, InputEntry input) {
        std::lock_guard<std::mutex> lk(mtx);
        inputHistory[frame][playerId] = input;
    }

    // Mark a player as connected (use on first connect or reconnect)
    void OnPlayerConnected(int playerId) {
        std::lock_guard<std::mutex> lk(mtx);
        connectedPlayers.insert(playerId);
    }

    // Call this when a player successfully reconnected.
    // This method fills any missing inputs for recent frames with a "zero" input
    // so the server doesn't detect a spurious rollback due to missing inputHistory
    // entries caused by the disconnect. It also marks the player as connected.
    void OnPlayerReconnected(int playerId) {
        std::lock_guard<std::mutex> lk(mtx);


        // mark connected
        connectedPlayers.insert(playerId);


        // find the latest snapshot frame we have
        int latestFrame = -1;
        for (const auto &s : snapshots) {
            if (s.frame > latestFrame) latestFrame = s.frame;
        }
        if (latestFrame < 0) return;


        // Fill missing inputs for the last MAX_ROLLBACK_FRAMES frames
        int start = (std::max)(0, latestFrame - MAX_ROLLBACK_FRAMES + 1);
        for (int f = start; f <= latestFrame; ++f) {
            auto &frameMap = inputHistory[f]; // creates if missing
        if (frameMap.find(playerId) == frameMap.end()) {
            InputEntry zero;
            zero.frame = f;
            zero.playerId = playerId;
            zero.input = MakeZeroInputBlob();
            frameMap[playerId] = zero;
        }
    }


    // Also ensure appliedInputs has entries for those frames so comparison
    // logic doesn't think we need to rollback due to size mismatch.
    for (int f = start; f <= latestFrame; ++f) {
    auto frameIt = inputHistory.find(f);
    if (frameIt != inputHistory.end()) {
    appliedInputs[f][playerId] = frameIt->second[playerId];
    }
    }
    }


    // Mark a player as disconnected
    void OnPlayerDisconnected(int playerId) {
    std::lock_guard<std::mutex> lk(mtx);
    connectedPlayers.erase(playerId);
    }



    InputEntry GetInputForPlayerAtFrame(int playerId, int frame) {
        std::lock_guard<std::mutex> lk(mtx);
        auto frameIt = inputHistory.find(frame);
        if (frameIt == inputHistory.end()) return InputEntry{frame, MakeZeroInputBlob(), playerId};
        
        auto playerIt = frameIt->second.find(playerId);
        return playerIt != frameIt->second.end() ? playerIt->second : InputEntry{frame, MakeZeroInputBlob(), playerId};
    }

    int GetSizeOfInputsAtFrame(int frame) {
        std::lock_guard<std::mutex> lk(mtx);
        auto frameIt = inputHistory.find(frame);
        if (frameIt == inputHistory.end()) return 0;
        return frameIt->second.size();
    }

    void SaveSnapshot(int frame) {
        std::lock_guard<std::mutex> lk(mtx);
        Snapshot s;
        s.frame = frame;
        s.state = gameState;
        int idx = frame % MAX_ROLLBACK_FRAMES;
        snapshots[idx] = s;
    }

    bool LoadSnapshot(int frame) {
        std::lock_guard<std::mutex> lk(mtx);
        int idx = frame % MAX_ROLLBACK_FRAMES;
        Snapshot &s = snapshots[idx];
        if (s.frame != frame) return false;
        gameState = s.state;
        return true;
    }

    void SimulateFrame(int frame) {
        std::map<int, InputEntry> inputs;

        auto frameIt = inputHistory.find(frame);
        if (frameIt != inputHistory.end())
        {
            inputs = frameIt->second;
        }

        gameLogic->SimulateFrame(gameState, inputs);

        gameState.frame = frame;
    }

    StateUpdate Tick(int currentFrame) {
        // Check if we need to rollback
        
        int rollbackTo = -1;
        {
            std::lock_guard<std::mutex> lk(mtx);
            int start = (std::max)(0, currentFrame - MAX_ROLLBACK_FRAMES);
            for (int f = start; f < currentFrame; ++f) {
                auto appliedIt = appliedInputs.find(f);
                auto actualIt = inputHistory.find(f);
                
                if (appliedIt == appliedInputs.end() && actualIt != inputHistory.end()) {
                    rollbackTo = f;
                    break;
                }
                
                if (appliedIt != appliedInputs.end() && actualIt != inputHistory.end()) {
                    const auto& appliedMap = appliedIt->second;
                    const auto& actualMap = actualIt->second;

                    // Check for size mismatch
                    if (appliedMap.size() != actualMap.size()) {
                        rollbackTo = f;
                        break;
                    }

                    // Check for key/value mismatch
                    for (const auto& [playerId, actualEntry] : actualMap) {
                        auto appliedEntryIt = appliedMap.find(playerId);
                        if (appliedEntryIt == appliedMap.end() || appliedEntryIt->second != actualEntry) {
                            rollbackTo = f;
                            break;
                        }
                    }
                    if (rollbackTo != -1) break;
                }
            }
        }

        if (rollbackTo != -1) {
            //std::cerr << "Server rolling back to frame " << rollbackTo << " from actual frame " << currentFrame << "\n";
            // Load snapshot from before rollback point
            int loadFrame = rollbackTo - 1;
            if (loadFrame < 0) loadFrame = 0;
            if (!LoadSnapshot(loadFrame)) {
                std::cerr << "Rollback failed: no snapshot for frame " << loadFrame << "\n";
            } else {
                // Clear applied inputs from rollback point onward
                auto it = appliedInputs.lower_bound(rollbackTo);
                appliedInputs.erase(it, appliedInputs.end());
                // Re-simulate from rollback point
                for (int f = loadFrame + 1; f <= currentFrame; ++f) {
                    SimulateFrame(f);
                    SaveSnapshot(f);
                    // Use iterators for appliedInputs
                    auto frameIt = inputHistory.find(f);
                    if (frameIt != inputHistory.end()) {
                        for (const auto& [playerId, inputEntry] : frameIt->second) {
                            appliedInputs[f][playerId] = inputEntry;
                        }
                    }
                }
            }
        }else
        {
            // Normal forward simulation
            SimulateFrame(currentFrame);
            SaveSnapshot(currentFrame);
        }

        
        // Use iterators for appliedInputs
        auto frameIt = inputHistory.find(currentFrame);
        if (frameIt != inputHistory.end()) {
            for (const auto& [playerId, inputEntry] : frameIt->second) {
                appliedInputs[currentFrame][playerId] = inputEntry;
            }
        }

        // Create state update
        StateUpdate update;
        update.frame = currentFrame;
        update.state = GetCurrentState();
        // Use iterators for confirmedInputs
        if (frameIt != inputHistory.end()) {
            for (const auto& [playerId, inputEntry] : frameIt->second) {
                update.confirmedInputs[playerId] = inputEntry;
            }
        }
        return update;
    }

    GameStateBlob GetCurrentState() {
        std::lock_guard<std::mutex> lk(mtx);
        return gameState;
    }

    void SetGameLogic(std::unique_ptr<IGameLogic> logic) {
        std::lock_guard<std::mutex> lk(mtx);
        gameLogic = std::move(logic);
        gameLogic->Init(gameState);
    }

    IGameLogic* GetGameLogic()
    {
        return gameLogic.get();
    }

private:
    struct Snapshot { 
        int frame = -1; 
        GameStateBlob state; 
    };

    std::mutex mtx;
    GameStateBlob gameState;
    std::unique_ptr<IGameLogic> gameLogic;
    std::vector<Snapshot> snapshots;
    InputHistory inputHistory;
    std::map<int, std::map<int, InputEntry>> appliedInputs; // frame -> applied inputs for both players

    std::set<int> connectedPlayers;
};

#endif // SERVER_NETCODE_H