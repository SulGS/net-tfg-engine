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
		Snapshot& snapshot = GetSnapshot(event.frame);
		snapshot.events.push_back(event);
	}

	void OnServerInputUpdate(const InputEntry& inputEntry) {
		std::lock_guard<std::mutex> lock(mtx);
		Snapshot& snapshot = GetSnapshot(inputEntry.frame);
		snapshot.inputs[inputEntry.playerId] = inputEntry;
	}

	void UpdateCurrentFrame(int framesAboveServer)
	{
		std::lock_guard<std::mutex> lock(mtx);
		framesAheadOfServer = framesAboveServer;
	}

	void OnServerDeltasUpdate(std::vector<DeltaStateBlob>& deltas, int& deltaFrame)
	{
		std::lock_guard<std::mutex>lock(mtx);
		Snapshot& snapshot = GetSnapshot(deltaFrame);
		lastConfirmedFrame = deltaFrame;
		snapshot.stateConfirmed = true;


		bool needsCorrection = false;

		if (!(gameLogic->CompareStateWithDeltas(snapshot.state, deltas)))
		{
			needsCorrection = true;
			gameLogic->ApplyDeltasToGameState(snapshot.state, deltas);
		}

		latestServerState = snapshot.state;
		latestServerState.frame = deltaFrame;

		if (needsCorrection)
		{
			currentFrame = lastConfirmedFrame + framesAheadOfServer;

			gameLogic->Synchronize(snapshot.state);

			for (int frame = deltaFrame; frame < currentFrame; ++frame) {
				SimulateFrame(frame, true);
			}

			Snapshot& lastSnapshot = GetSnapshot(currentFrame);
			currentState.len = lastSnapshot.state.len;
			memcpy(currentState.data, lastSnapshot.state.data, currentState.len);

			Debug::Info("ClientNetcode") << "[CLIENT] Reconciled to server state at frame " << deltaFrame
				<< ". Current frame: " << currentFrame << "\n";

		}

		RemoveYetConfirmedSnapshots();
	}

	void OnServerStateUpdate(const StateUpdate& update)
	{
		std::lock_guard<std::mutex>lock(mtx);

		Snapshot& snapshot = GetSnapshot(update.frame);
		lastConfirmedFrame = update.frame;
		snapshot.stateConfirmed = true;
		latestServerState = update.state;
		latestServerState.frame = update.frame;




		if (gameLogic->CompareStates(snapshot.state, update.state))
		{
			RemoveYetConfirmedSnapshots();
			return; // No reconciliation needed
		}

		currentFrame = lastConfirmedFrame + framesAheadOfServer;

		snapshot.state.len = update.state.len;
		if (snapshot.state.len > sizeof(snapshot.state.data))
			snapshot.state.len = sizeof(snapshot.state.data);
		memcpy(snapshot.state.data, update.state.data, snapshot.state.len);

		gameLogic->Synchronize(snapshot.state);

		for (int frame = update.frame; frame < currentFrame; ++frame) {
			SimulateFrame(frame, true);
		}

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

		SimulateFrame(currentFrame, false);
		currentFrame++;

		Snapshot& predictedSnapshot = GetSnapshot(currentFrame);
		currentState = predictedSnapshot.state;
		currentState.frame = currentFrame;


	}

	void SetGameLogic(std::unique_ptr<IGameLogic> logic) {
		std::lock_guard<std::mutex> lock(mtx);
		gameLogic = std::move(logic);
		gameLogic->isServer = false;
		gameLogic->Init(currentState);
		currentState.frame = 0;
		currentFrame = 0;
		lastConfirmedFrame = 0;
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

	// Transfers gameLogic ownership back to the caller; call before deleting this object if the logic must survive for the next session.
	std::unique_ptr<IGameLogic> ReleaseGameLogic() {
		std::lock_guard<std::mutex> lock(mtx);
		return std::move(gameLogic);
	}

private:

	std::unique_ptr<IGameLogic> gameLogic;
	int localPlayerId;

	GameStateBlob currentState;
	GameStateBlob latestServerState;
	int currentFrame = 0;
	int lastConfirmedFrame = 0;
	int framesAheadOfServer = 0;

	std::map<int, Snapshot> snapshots;

	mutable std::mutex mtx;

	Snapshot& GetSnapshot(int frame)
	{

		if (snapshots.find(frame) == snapshots.end())
		{
			snapshots[frame] = Snapshot();

			snapshots[frame].frame = frame;
			if (frame > 0)
			{
				snapshots[frame].state = snapshots[frame - 1].state;
			}
			else
			{
				snapshots[frame].state = currentState;
			}


		}

		return snapshots[frame];
	}

	void SimulateFrame(int frame, bool debug)
	{
		Snapshot& currentSnapshot = GetSnapshot(frame);

		gameLogic->Synchronize(currentSnapshot.state);

		GameStateBlob stateToSimulate;

		gameLogic->SimulateFrame(stateToSimulate, currentSnapshot.events, currentSnapshot.inputs);

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