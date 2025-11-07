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

	void OnServerEventUpdate(const EventEntry& event)
	{
		WithSnapshot(event.frame, [&](SNAPSHOT& snap) {
                snap.events.push_back(event);
			});
	}

    void OnServerStateUpdate(const StateUpdate& update) {
        bool needCorrection = false;

        WithSnapshot(update.frame, [&](SNAPSHOT& snap) {
            const GameStateBlob& ourState = snap.state;
            if (!gameLogic->CompareStates(ourState, update.state)) {
                needCorrection = true;
                std::cerr << "State misprediction detected at server frame " << update.frame << "\n";
            }

            // Store server state
            snap.state = update.state;
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

    void OnServerFrameUpdate(const FrameUpdate& update) {
        bool needCorrection = false;

        WithSnapshot(update.frame, [&](SNAPSHOT& snap) {

            // Print sizes
			std::cout << "[DEBUG] Frame " << update.frame
				<< ": snap.inputs.size()=" << snap.inputs.size()
				<< ", update.inputs.size()=" << update.inputs.size()
				<< ", snap.events.size()=" << snap.events.size()
				<< ", update.events.size()=" << update.events.size() << "\n";

            // Verifica si inputs o eventos difieren
            bool eventsDifferent =
                (snap.events.size() != update.events.size()) ||
                !std::is_permutation(snap.events.begin(), snap.events.end(), update.events.begin());

            if (snap.inputs != update.inputs) {
                needCorrection = true;
                std::cerr << "Input misprediction detected at server frame "
                    << update.frame << "\n";
            }

            if (eventsDifferent) {
                needCorrection = true;
                std::cerr << "Event misprediction detected at server frame "
                    << update.frame << "\n";
            }

            // Actualiza solo inputs y eventos con los del servidor (no todo el snapshot)
            snap.inputs = update.inputs;
            snap.events = update.events;
            });

        // Si hay desincronización y el frame es anterior o igual al actual, re-predice desde ese frame
        if (needCorrection && update.frame <= currentPredictionFrame) {
            WithSnapshot(update.frame, [&](SNAPSHOT& snap) {
                gameState = snap.state;
            });
            for (int f = update.frame; f <= currentPredictionFrame; ++f) {
                PredictFrame(f);
            }

            std::cerr << "Applied input/event correction from frame " << update.frame
                << " to " << currentPredictionFrame << "\n";
        }
        // Si el frame es más reciente, simplemente avanzamos
        else if (update.frame > currentPredictionFrame) {
            currentPredictionFrame = update.frame;
        }
    }


    void PredictFrame(int frame) {
        std::map<int, InputEntry> inputs;
		std::vector<EventEntry> events;
        // copy inputs while holding the lock to ensure consistent view
        WithSnapshot(frame, [&](SNAPSHOT& snap) {
            inputs = snap.inputs;
            events = snap.events;
            });

        // Simulate frame (do NOT hold snapshots lock while simulating)
        gameLogic->SimulateFrame(gameState, events, inputs);
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
		gameLogic->isServer = false;
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