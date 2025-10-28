#ifndef CLIENT_NETCODE_H
#define CLIENT_NETCODE_H
#include "netcode_common.hpp"
#include <functional>
#include <deque>

// Client-side prediction netcode
class ClientPredictionNetcode {
public:
    ClientPredictionNetcode(int playerId, std::unique_ptr<IGameLogic> logic) : localPlayerId(playerId) {
        snapshots.resize(MAX_ROLLBACK_FRAMES*2);
        SetGameLogic(std::move(logic));
    }

    // Replace GetSnapshot(...) with this:
    template<typename F>
    void WithSnapshot(int frame, F&& fn) {
        // search for existing snapshot
        for (auto it = snapshots.begin(); it != snapshots.end(); ++it) {
            if (it->frame == frame) {
                fn(*it);           // operate on real snapshot while lock held
                return;
            }
        }
        // not found -> create, set its frame, then call fn
        snapshots.emplace_back(SNAPSHOT{});   // default-constructed
        snapshots.back().frame = frame;      // IMPORTANT: set the frame
        fn(snapshots.back());
    }



    void SubmitLocalInput(int frame, InputBlob input) {
        // we don't need an extra lock here because WithSnapshot locks internally
        WithSnapshot(frame, [&](SNAPSHOT& snap) {
            snap.inputs[localPlayerId] = { frame, input, localPlayerId };
            });
    }

    void OnServerInputUpdate(InputEntry ie)
    {
        WithSnapshot(ie.frame, [&](SNAPSHOT& snap) {
            snap.inputs[ie.playerId] = ie;
            });
    }

    void OnServerStateUpdate(const StateUpdate& update) {
        bool needCorrection = false;

        WithSnapshot(update.frame, [&](SNAPSHOT& snap) {
            const GameStateBlob& ourState = snap.state;
            if (!gameLogic->CompareStates(ourState, update.state)) {
                needCorrection = true;
                std::cerr << "Misprediction detected at server frame " << update.frame << "\n";
            }

            // Store server state
            snap.state = update.state;
            snap.inputs = update.confirmedInputs;
            });

        // NOTE: we intentionally perform the heavier correction / prediction
        // outside snapshot lock to avoid holding mutex while simulating.
        if (needCorrection && update.frame <= currentPredictionFrame) {
            gameState = update.state;
            gameState.frame = update.frame;
            for (int f = update.frame + 1; f <= currentPredictionFrame; ++f) {
                PredictFrame(f);
            }
            std::cerr << "Applied correction from frame " << update.frame
                << " to " << currentPredictionFrame << "\n";
        }
        else if (update.frame >= currentPredictionFrame) {
            gameState = update.state;
            currentPredictionFrame = update.frame;
        }
    }

    void PredictFrame(int frame) {
        std::map<int, InputEntry> inputs;
        // copy inputs while holding the lock to ensure consistent view
        WithSnapshot(frame, [&](SNAPSHOT& snap) {
            inputs = snap.inputs;
            });

        // Simulate frame (do NOT hold snapshots lock while simulating)
        gameLogic->SimulateFrame(gameState, inputs);
        gameState.frame = frame;

        // store resulting state back into snapshot
        WithSnapshot(frame, [&](SNAPSHOT& snap) {
            snap.state = gameState;
            });
    }

    void PredictToFrame(int targetFrame) {
        std::lock_guard<std::mutex> lk(mtx);
        
        for (int f = currentPredictionFrame + 1; f <= targetFrame; ++f) {
            PredictFrame(f);
        }
        currentPredictionFrame = targetFrame;

        currentPredictionFrame = targetFrame;
        // prune old history, keep last 2*MAX_ROLLBACK_FRAMES frames
        int keepFrom = (std::max)(0, currentPredictionFrame - (MAX_ROLLBACK_FRAMES));
        PruneHistoryBefore(keepFrom);
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

    IGameLogic* GetGameLogic() const {
        return gameLogic.get();
    }

private:
    
    std::unique_ptr<IGameLogic> gameLogic;
    int localPlayerId;
    int currentPredictionFrame = 0;
    std::mutex mtx;
    GameStateBlob gameState;
    std::deque<SNAPSHOT> snapshots;

    // Keep only frames >= keepFrom in all history maps
    void PruneHistoryBefore(int keepFrom) {
        // confirmedSnapshots
        for (auto it = snapshots.begin(); it != snapshots.end();) {
            if (it->frame < keepFrom) it = snapshots.erase(it);
            else ++it;
        }
    }

    
};
#endif // CLIENT_NETCODE_H