#ifndef CLIENT_NETCODE_H
#define CLIENT_NETCODE_H
#include "netcode_common.hpp"
#include "Utils/Debug/Debug.hpp"
#include <functional>
#include <map>
#include <deque>
#include <algorithm>

// Client-side prediction with server reconciliation and lag compensation
class ClientPredictionNetcode {
public:
	ClientPredictionNetcode(int playerId, std::unique_ptr<IGameLogic> logic)
		: localPlayerId(playerId) {
		SetGameLogic(std::move(logic));
	}

	int SubmitLocalInput(const InputBlob& input)
	{
		std::lock_guard<std::mutex> lock(mtx);
		Snapshot& currentSnapshot = GetSnapshot(currentFrame);
		currentSnapshot.inputs[localPlayerId] = InputEntry{ currentFrame, input, localPlayerId };

		return currentFrame;
	}

	void OnServerEventUpdate(const EventEntry& event)
	{
		std::lock_guard<std::mutex> lock(mtx);
		// Store the server-sent event in the corresponding snapshot
		Snapshot& snapshot = GetSnapshot(event.frame);
		snapshot.events.push_back(event);
	}

	void OnServerInputUpdate(const InputEntry& inputEntry) {
		std::lock_guard<std::mutex> lock(mtx);
		// Store the server-confirmed input in the corresponding snapshot
		Snapshot& snapshot = GetSnapshot(inputEntry.frame);
		snapshot.inputs[inputEntry.playerId] = inputEntry;
	}

	void UpdateCurrentFrame(int framesAboveServer)
	{
		std::lock_guard<std::mutex> lock(mtx);
		framesAheadOfServer = framesAboveServer;
	}

	void OnServerStateUpdate(const StateUpdate& update)
	{
		std::lock_guard<std::mutex>lock(mtx);

		Snapshot& snapshot = GetSnapshot(update.frame);
		lastConfirmedFrame = update.frame;
		snapshot.stateConfirmed = true;
		latestServerState = update.state;

		if (gameLogic->CompareStates(snapshot.state, update.state) )
		{
			return; // No reconciliation needed
		}

		currentFrame = lastConfirmedFrame + framesAheadOfServer;

		// Copy server state safely into snapshot
		snapshot.state.len = update.state.len;
		if (snapshot.state.len > sizeof(snapshot.state.data))
			snapshot.state.len = sizeof(snapshot.state.data);
		memcpy(snapshot.state.data, update.state.data, snapshot.state.len);

		gameLogic->Synchronize(snapshot.state);

		// Re-simulate all frames after the server frame
		for (int frame = update.frame; frame < currentFrame; ++frame) {
			SimulateFrame(frame, true);
		}

		// Update current client state from the last predicted snapshotQ
		Snapshot& lastSnapshot = GetSnapshot(currentFrame);
		currentState.len = lastSnapshot.state.len;
		memcpy(currentState.data, lastSnapshot.state.data, currentState.len);

		Debug::Info("ClientNetcode") << "[CLIENT] Reconciled to server state at frame " << update.frame
			<< ". Current frame: " << currentFrame << "\n";

		RemoveYetConfirmedSnapshots();

	}

	void Tick()
	{
		std::lock_guard<std::mutex> lock(mtx);
		// Store local input in the current snapshot
		Snapshot& currentSnapshot = GetSnapshot(currentFrame);
		currentSnapshot.frame = currentFrame;

		SimulateFrame(currentFrame, false);
		currentFrame++;

		Snapshot& predictedSnapshot = GetSnapshot(currentFrame);
		currentState = predictedSnapshot.state;
	}

	void SetGameLogic(std::unique_ptr<IGameLogic> logic) {
		std::lock_guard<std::mutex> lock(mtx);
		gameLogic = std::move(logic);
		gameLogic->isServer = false;
		gameLogic->Init(currentState);
		currentState.frame = 0;
		currentFrame = 0;
		lastConfirmedFrame = 0;
		//Create initial snapshot
		Snapshot& initSnapshot = GetSnapshot(0);

	}

	GameStateBlob GetCurrentState() const {
		std::lock_guard<std::mutex> lock(mtx);
		return currentState;
	}

	GameStateBlob GetLatestServerState() const {
		std::lock_guard<std::mutex> lock(mtx);
		return latestServerState;
	}

	IGameLogic* GetGameLogic() const {
		return gameLogic.get();
	}

private:

	std::unique_ptr<IGameLogic> gameLogic;
	int localPlayerId;

	GameStateBlob currentState;
	GameStateBlob latestServerState;
	int currentFrame = 0;           // Current client frame
	int lastConfirmedFrame = 0;
	int framesAheadOfServer = 0;

	std::map<int, Snapshot> snapshots;

	mutable std::mutex mtx;

	Snapshot& GetSnapshot(int frame)
	{

		if (snapshots.find(frame) == snapshots.end())
		{
			// Create default snapshot
			snapshots[frame] = Snapshot();

			snapshots[frame].frame = frame;
			if (frame > 0)
			{
				// Copy state from previous frame
				snapshots[frame].state = snapshots[frame - 1].state;
			}
			else
			{
				// Initial state
				snapshots[frame].state = currentState;
			}


		}

		return snapshots[frame];
	}

	void SimulateFrame(int frame, bool debug)
	{
		// Get the snapshot for this frame
		Snapshot& currentSnapshot = GetSnapshot(frame);

		gameLogic->Synchronize(currentSnapshot.state);

		// Create a fresh, deterministic copy of the state
		GameStateBlob stateToSimulate;

		// Simulate deterministically: write into our local copy
		gameLogic->SimulateFrame(stateToSimulate, currentSnapshot.events, currentSnapshot.inputs);

		// Save the result back into the next snapshot
		Snapshot& predictedSnapshot = GetSnapshot(frame + 1);
		predictedSnapshot.frame = frame + 1;
		predictedSnapshot.state = stateToSimulate;
		predictedSnapshot.stateConfirmed = false;
	}

	void RemoveYetConfirmedSnapshots()
	{
		for (auto it = snapshots.begin(); it != snapshots.end(); ) {
			if (it->first < lastConfirmedFrame) {
				it = snapshots.erase(it);
			}
			else {
				++it;
			}
		}
	}


};

#endif // CLIENT_NETCODE_H