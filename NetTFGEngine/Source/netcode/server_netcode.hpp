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

    }


    // Mark a player as disconnected
    void OnPlayerDisconnected(int playerId) {
        std::lock_guard<std::mutex> lk(mtx);
        connectedPlayers.erase(playerId);
    }



    InputEntry GetInputForPlayerAtFrame(int playerId, int frame) {
        std::lock_guard<std::mutex> lk(mtx);
        auto frameIt = appliedInputs.find(frame);
        if (frameIt == appliedInputs.end()) return InputEntry{frame, MakeZeroInputBlob(), playerId};
        
        auto playerIt = frameIt->second.find(playerId);
        return playerIt != frameIt->second.end() ? playerIt->second : InputEntry{frame, MakeZeroInputBlob(), playerId};
    }

    int GetSizeOfInputsAtFrame(int frame) {
        std::lock_guard<std::mutex> lk(mtx);
        auto frameIt = appliedInputs.find(frame);
        if (frameIt == appliedInputs.end()) return 0;
        return frameIt->second.size();
    }

    void SimulateFrame(int frame) {
        std::map<int, InputEntry> inputs;

        auto frameInIt = appliedInputs.find(frame);
        if (frameInIt != appliedInputs.end())
        {
            inputs = frameInIt->second;
        }

		std::vector<EventEntry> events;
        auto frameEvIt = appliedEvents.find(frame);
        if (frameEvIt != appliedEvents.end())
        {
            events = frameEvIt->second;
        }


        gameLogic->SimulateFrame(gameState,events, inputs);

		for (auto& event : gameLogic->generatedEvents)
		{
			event.frame = frame + 1;
		}

		appliedEvents[frame+1] = gameLogic->generatedEvents;

        gameState.frame = frame;
    }

    StateUpdate Tick(int currentFrame) {
        
        SimulateFrame(currentFrame);

        // Create state update
        StateUpdate update;
        update.frame = currentFrame;
		update.state = gameState;



        return update;
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
    }

    IGameLogic* GetGameLogic()
    {
        return gameLogic.get();
    }

private:

    std::mutex mtx;
    GameStateBlob gameState;
    std::unique_ptr<IGameLogic> gameLogic;
    InputHistory appliedInputs; 
	EventsHistory appliedEvents; 

    std::set<int> connectedPlayers;
};

#endif // SERVER_NETCODE_H