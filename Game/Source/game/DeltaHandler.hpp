#pragma once
#include "ecs/Deltas/ecs_iecs_delta_handler.hpp"
#include "ecs/ecs_common.hpp"
#include "GameState.hpp"
#include "Deltas.hpp"
#include <cstring>

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

// Walls/tiles aren't predicted client-side (ArenaSystem only runs on the
// server) — this delta is the client's only source of truth for them.
// Sending the full boolean grid every tick (like GamePositionsDeltaHandler
// does for positions) means any divergence self-heals on the very next tick
// instead of leaving the client stuck showing a wall as on/off.
class WallStateDeltaHandler : public IDeltaHandler {
public:
	void Apply(const DeltaStateBlob& delta, GameStateBlob& currentState) override
	{
		const WallStateDelta* wsd = reinterpret_cast<const WallStateDelta*>(delta.data);
		AsteroidShooterGameState* gs = reinterpret_cast<AsteroidShooterGameState*>(currentState.data);

		std::memcpy(gs->tilesActive, wsd->tilesActive, sizeof(gs->tilesActive));
		std::memcpy(gs->tilesWarning, wsd->tilesWarning, sizeof(gs->tilesWarning));
		std::memcpy(gs->hWalls, wsd->hWalls, sizeof(gs->hWalls));
		std::memcpy(gs->vWalls, wsd->vWalls, sizeof(gs->vWalls));
		std::memcpy(gs->cWalls, wsd->cWalls, sizeof(gs->cWalls));
		std::memcpy(gs->hWallsWarning, wsd->hWallsWarning, sizeof(gs->hWallsWarning));
		std::memcpy(gs->vWallsWarning, wsd->vWallsWarning, sizeof(gs->vWallsWarning));
		std::memcpy(gs->cWallsWarning, wsd->cWallsWarning, sizeof(gs->cWallsWarning));
	}

	void Check(const GameStateBlob& prevState,
		const GameStateBlob& currentState,
		std::vector<DeltaStateBlob>& outDeltas) override
	{
		const AsteroidShooterGameState* currGS = reinterpret_cast<const AsteroidShooterGameState*>(currentState.data);

		WallStateDelta wsd;
		std::memcpy(wsd.tilesActive, currGS->tilesActive, sizeof(wsd.tilesActive));
		std::memcpy(wsd.tilesWarning, currGS->tilesWarning, sizeof(wsd.tilesWarning));
		std::memcpy(wsd.hWalls, currGS->hWalls, sizeof(wsd.hWalls));
		std::memcpy(wsd.vWalls, currGS->vWalls, sizeof(wsd.vWalls));
		std::memcpy(wsd.cWalls, currGS->cWalls, sizeof(wsd.cWalls));
		std::memcpy(wsd.hWallsWarning, currGS->hWallsWarning, sizeof(wsd.hWallsWarning));
		std::memcpy(wsd.vWallsWarning, currGS->vWallsWarning, sizeof(wsd.vWallsWarning));
		std::memcpy(wsd.cWallsWarning, currGS->cWallsWarning, sizeof(wsd.cWallsWarning));

		DeltaStateBlob deltaBlob;
		deltaBlob.delta_type = DELTA_WALL_STATE;
		std::memcpy(deltaBlob.data, &wsd, sizeof(WallStateDelta));
		deltaBlob.len = sizeof(WallStateDelta);
		outDeltas.push_back(deltaBlob);
	}

	bool Compare(const DeltaStateBlob& delta,
		const GameStateBlob& currentState) override
	{
		const WallStateDelta* wsd = reinterpret_cast<const WallStateDelta*>(delta.data);
		const AsteroidShooterGameState* gs = reinterpret_cast<const AsteroidShooterGameState*>(currentState.data);

		if (std::memcmp(wsd->tilesActive, gs->tilesActive, sizeof(wsd->tilesActive)) != 0) return false;
		if (std::memcmp(wsd->tilesWarning, gs->tilesWarning, sizeof(wsd->tilesWarning)) != 0) return false;
		if (std::memcmp(wsd->hWalls, gs->hWalls, sizeof(wsd->hWalls)) != 0) return false;
		if (std::memcmp(wsd->vWalls, gs->vWalls, sizeof(wsd->vWalls)) != 0) return false;
		if (std::memcmp(wsd->cWalls, gs->cWalls, sizeof(wsd->cWalls)) != 0) return false;
		if (std::memcmp(wsd->hWallsWarning, gs->hWallsWarning, sizeof(wsd->hWallsWarning)) != 0) return false;
		if (std::memcmp(wsd->vWallsWarning, gs->vWallsWarning, sizeof(wsd->vWallsWarning)) != 0) return false;
		if (std::memcmp(wsd->cWallsWarning, gs->cWallsWarning, sizeof(wsd->cWallsWarning)) != 0) return false;

		return true;
	}
};