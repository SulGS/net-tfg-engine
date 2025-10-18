#ifndef CLIENT_NETCODE_H
#define CLIENT_NETCODE_H
#include "netcode_common.hpp"
#include <functional>

// Client-side prediction netcode
class ClientPredictionNetcode {
public:
    ClientPredictionNetcode(int playerId, std::unique_ptr<IGameLogic> logic) : localPlayerId(playerId) {
        snapshots.resize(MAX_ROLLBACK_FRAMES);
        SetGameLogic(std::move(logic));
        SaveSnapshot(0);
    }

    void SubmitLocalInput(int frame, InputBlob input) {
        std::lock_guard<std::mutex> lk(mtx);
        localInputs[frame] = {frame, input, localPlayerId};
        confirmedInputs[frame][localPlayerId] = {frame, input, localPlayerId};
    }

    void OnServerInputUpdate(InputEntry ie)
    {
        confirmedInputs[ie.frame][ie.playerId] = ie;
    }

    void OnServerStateUpdate(const StateUpdate& update) {
        std::lock_guard<std::mutex> lk(mtx);
        //std::cerr << "Client received state update for frame " << update.frame << "\n";
        // Check if we need to correct our prediction
        bool needCorrection = false;
        // Find our snapshot for this frame
        auto snapshotIt = confirmedSnapshots.find(update.frame);
        if (snapshotIt == confirmedSnapshots.end()) {
            needCorrection = true;
        } else {
            // Compare states
            const GameStateBlob& ourState = snapshotIt->second;
            if (!gameLogic->CompareStates(ourState, update.state)) {
                needCorrection = true;
                std::cerr << "Misprediction detected at server frame " << update.frame << "\n";
            }
        }
        // Store server state
        serverStates[update.frame] = update.state;
        confirmedInputs[update.frame] = update.confirmedInputs;
        // Apply correction if needed
        if (needCorrection && update.frame <= currentPredictionFrame) {
            // Rollback and re-predict
            gameState = update.state;
            gameState.frame = update.frame;
            // Re-simulate from server frame to current prediction
            for (int f = update.frame + 1; f <= currentPredictionFrame; ++f) {
                PredictFrame(f);
            }
            std::cerr << "Applied correction from frame " << update.frame 
                      << " to " << currentPredictionFrame << "\n";
        } else if (update.frame >= currentPredictionFrame) {
            // Server is at or ahead, adopt server state
            gameState = update.state;
            currentPredictionFrame = update.frame;
        }
    }

    void PredictFrame(int frame) {

        std::map<int, InputEntry> inputs;
        
        // Use confirmed input from server if available
        auto confirmedIt = confirmedInputs.find(frame);
        if (confirmedIt != confirmedInputs.end()) {
            inputs = confirmedIt->second;
        } else {
            auto lastConfirmedIt = confirmedInputs.rbegin();
            if (lastConfirmedIt != confirmedInputs.rend()) {
                inputs = lastConfirmedIt->second;
            }

            // Only use a local input for this frame if we've actually submitted one.
            auto lit = localInputs.find(frame);
            if (lit != localInputs.end()) {
                inputs[localPlayerId] = lit->second;
            } else {
                // If we don't have a local input for this frame, fall back to zero input.
                inputs[localPlayerId] = InputEntry{frame, MakeZeroInputBlob(), localPlayerId};
            }
        }

        

        // Simulate frame
        gameLogic->SimulateFrame(gameState, inputs);

        gameState.frame = frame;
        
        // Save snapshot of our prediction
        confirmedSnapshots[frame] = gameState;
    }

    void PredictToFrame(int targetFrame) {
        std::lock_guard<std::mutex> lk(mtx);
        
        for (int f = currentPredictionFrame + 1; f <= targetFrame; ++f) {
            PredictFrame(f);
        }
        currentPredictionFrame = targetFrame;

        currentPredictionFrame = targetFrame;
        // prune old history, keep last 2*MAX_ROLLBACK_FRAMES frames
        int keepFrom = (std::max)(0, currentPredictionFrame - (2 * MAX_ROLLBACK_FRAMES));
        PruneHistoryBefore(keepFrom);
    }

    GameStateBlob GetCurrentState() {
        std::lock_guard<std::mutex> lk(mtx);
        return gameState;
    }

    void SaveSnapshot(int frame) {
        // Not needed for client prediction, keeping for compatibility
    }

    void SetGameLogic(std::unique_ptr<IGameLogic> logic) {
        std::lock_guard<std::mutex> lk(mtx);
        gameLogic = std::move(logic);
        gameLogic->Init(gameState);
    }

    IGameLogic* GetGameLogic() const {
        return gameLogic.get();
    }

private:
    
    std::unique_ptr<IGameLogic> gameLogic;
    int localPlayerId;
    int currentPredictionFrame = 0;
    std::mutex mtx;
    GameStateBlob gameState;
    std::vector<SNAPSHOT> snapshots; // Keep for compatibility
    std::map<int, InputEntry> localInputs;
    std::map<int, GameStateBlob> serverStates;
    std::map<int, GameStateBlob> confirmedSnapshots;
    std::map<int, std::map<int, InputEntry>> confirmedInputs; // frame -> confirmed inputs for both players

    // Keep only frames >= keepFrom in all history maps
    void PruneHistoryBefore(int keepFrom) {
        // localInputs
        for (auto it = localInputs.begin(); it != localInputs.end();) {
            if (it->first < keepFrom) it = localInputs.erase(it);
            else ++it;
        }
        // confirmedSnapshots
        for (auto it = confirmedSnapshots.begin(); it != confirmedSnapshots.end();) {
            if (it->first < keepFrom) it = confirmedSnapshots.erase(it);
            else ++it;
        }
        // serverStates
        for (auto it = serverStates.begin(); it != serverStates.end();) {
            if (it->first < keepFrom) it = serverStates.erase(it);
            else ++it;
        }
        // confirmedInputs
        for (auto it = confirmedInputs.begin(); it != confirmedInputs.end();) {
            if (it->first < keepFrom) it = confirmedInputs.erase(it);
            else ++it;
        }
    }

    
};
#endif // CLIENT_NETCODE_H