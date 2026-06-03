#pragma once
#include "ecs/Deltas/ecs_iecs_delta_handler.hpp"
#include "ecs/ecs_common.hpp"
#include "GameState.hpp"
#include "Deltas.hpp"

class GamePositionsDeltaHandler : public IDeltaHandler {
public:
	void Apply(const DeltaStateBlob& delta, GameStateBlob& currentState) override
	{
		GamePositionsDelta gpd = *reinterpret_cast<const GamePositionsDelta*>(delta.data);
		AsteroidShooterGameState* gs = reinterpret_cast<AsteroidShooterGameState*>(currentState.data);

		for (int i = 0; i < NUM_PLAYERS; i++)
		{
			gs->posX[i] = gpd.posX[i];
			gs->posY[i] = gpd.posY[i];
			gs->rot[i] = gpd.rot[i];
		}
	}
	void Check(const GameStateBlob& prevState,
		const GameStateBlob& currentState,
		std::vector<DeltaStateBlob>& outDeltas) override
	{
		// Always send positions every tick so interpolation always has
		// prev and curr data, even when players are stationary.
		AsteroidShooterGameState currGS = *reinterpret_cast<const AsteroidShooterGameState*>(currentState.data);
		GamePositionsDelta gpd;

		for (int i = 0; i < NUM_PLAYERS; i++)
		{
			gpd.posX[i] = currGS.posX[i];
			gpd.posY[i] = currGS.posY[i];
			gpd.rot[i] = currGS.rot[i];
		}

		DeltaStateBlob deltaBlob;
		deltaBlob.delta_type = DELTA_GAME_POSITIONS;
		std::memcpy(deltaBlob.data, &gpd, sizeof(GamePositionsDelta));
		deltaBlob.len = sizeof(GamePositionsDelta);
		outDeltas.push_back(deltaBlob);
	}

	bool Compare(const DeltaStateBlob& delta,
		const GameStateBlob& currentState) override
	{
		GamePositionsDelta gpd = *reinterpret_cast<const GamePositionsDelta*>(delta.data);
		AsteroidShooterGameState gs = *reinterpret_cast<const AsteroidShooterGameState*>(currentState.data);

		for (int i = 0; i < NUM_PLAYERS; i++)
		{
			if (gs.posX[i] != gpd.posX[i]) return false;
			if (gs.posY[i] != gpd.posY[i]) return false;
			if (gs.rot[i] != gpd.rot[i]) return false;
		}

		return true;
	}

};