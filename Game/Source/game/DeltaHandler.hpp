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
		gs->startCountdownTicks = gpd.startCountdownTicks;
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
		gpd.startCountdownTicks = currGS.startCountdownTicks;

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
		if (gs.startCountdownTicks != gpd.startCountdownTicks) return false;

		return true;
	}

};

// Walls/tiles aren't predicted client-side (ArenaSystem only runs on the
// server) — this delta is the client's only source of truth for them.
//
// Wire format written into DeltaStateBlob::data (NOT a flat struct memcpy —
// deltaBlob.len is what actually gets put on the wire, see SendDeltasUpdate,
// so a short payload really does cost fewer bytes):
//   byte 0: mode — 0 = full snapshot, 1 = sparse change list
//   mode 0: WallStateDelta (the old always-sent format), at data+1
//   mode 1: byte 1 = change count N, then N x WallEdgeChange at data+2
//
// Now that every shared edge is a single entity (no more independently-
// toggled duplicate — see ClassifyWallEdge in Components.hpp), the server's
// state can only change a handful of edges per tick, so most ticks send a
// tiny sparse list, and many send nothing at all for this delta type.
// A full snapshot still goes out periodically (kFullSnapshotIntervalTicks)
// as a keyframe, and immediately if a single tick changes more edges than
// the sparse budget — both purely as defence in depth (e.g. against the
// client bridging to a frame it never locally simulated and starting from a
// blank state — see ClientPredictionNetcode::GetSnapshot), not because
// correctness depends on it the way the old always-full format did.
class WallStateDeltaHandler : public IDeltaHandler {
	static constexpr int kFullSnapshotIntervalTicks = 90; // ~3s at 30 TPS
	int ticksSinceFullSnapshot = kFullSnapshotIntervalTicks; // force one on the first Check()

	static void WriteFullSnapshot(DeltaStateBlob& deltaBlob, const AsteroidShooterGameState* gs)
	{
		WallStateDelta wsd;
		std::memcpy(wsd.tilesActive, gs->tilesActive, sizeof(wsd.tilesActive));
		std::memcpy(wsd.tilesWarning, gs->tilesWarning, sizeof(wsd.tilesWarning));
		std::memcpy(wsd.hWalls, gs->hWalls, sizeof(wsd.hWalls));
		std::memcpy(wsd.vWalls, gs->vWalls, sizeof(wsd.vWalls));
		std::memcpy(wsd.cWalls, gs->cWalls, sizeof(wsd.cWalls));
		std::memcpy(wsd.hWallsWarning, gs->hWallsWarning, sizeof(wsd.hWallsWarning));
		std::memcpy(wsd.vWallsWarning, gs->vWallsWarning, sizeof(wsd.vWallsWarning));
		std::memcpy(wsd.cWallsWarning, gs->cWallsWarning, sizeof(wsd.cWallsWarning));

		deltaBlob.data[0] = 0;
		std::memcpy(deltaBlob.data + 1, &wsd, sizeof(WallStateDelta));
		deltaBlob.len = 1 + (int)sizeof(WallStateDelta);
	}

public:
	void Apply(const DeltaStateBlob& delta, GameStateBlob& currentState) override
	{
		AsteroidShooterGameState* gs = reinterpret_cast<AsteroidShooterGameState*>(currentState.data);
		if (delta.len < 1) return;

		if (delta.data[0] == 0)
		{
			if ((size_t)delta.len < 1 + sizeof(WallStateDelta)) return; // malformed, ignore
			const WallStateDelta* wsd = reinterpret_cast<const WallStateDelta*>(delta.data + 1);
			std::memcpy(gs->tilesActive, wsd->tilesActive, sizeof(gs->tilesActive));
			std::memcpy(gs->tilesWarning, wsd->tilesWarning, sizeof(gs->tilesWarning));
			std::memcpy(gs->hWalls, wsd->hWalls, sizeof(gs->hWalls));
			std::memcpy(gs->vWalls, wsd->vWalls, sizeof(gs->vWalls));
			std::memcpy(gs->cWalls, wsd->cWalls, sizeof(gs->cWalls));
			std::memcpy(gs->hWallsWarning, wsd->hWallsWarning, sizeof(gs->hWallsWarning));
			std::memcpy(gs->vWallsWarning, wsd->vWallsWarning, sizeof(gs->vWallsWarning));
			std::memcpy(gs->cWallsWarning, wsd->cWallsWarning, sizeof(gs->cWallsWarning));
			return;
		}

		if (delta.len < 2) return;
		uint8_t count = delta.data[1];
		size_t needed = 2 + (size_t)count * sizeof(WallEdgeChange);
		if ((size_t)delta.len < needed) return; // malformed, ignore

		const WallEdgeChange* changes = reinterpret_cast<const WallEdgeChange*>(delta.data + 2);
		for (uint8_t i = 0; i < count; i++)
		{
			const WallEdgeChange& c = changes[i];
			switch (c.kind)
			{
			case WallEdgeKind::Tile:
				gs->tilesActive[c.x][c.y] = c.enabled;
				gs->tilesWarning[c.x][c.y] = c.warning;
				break;
			case WallEdgeKind::HWall:
				gs->hWalls[c.x][c.y] = c.enabled;
				gs->hWallsWarning[c.x][c.y] = c.warning;
				break;
			case WallEdgeKind::VWall:
				gs->vWalls[c.x][c.y] = c.enabled;
				gs->vWallsWarning[c.x][c.y] = c.warning;
				break;
			case WallEdgeKind::CWall:
				gs->cWalls[c.x][c.y][c.z] = c.enabled;
				gs->cWallsWarning[c.x][c.y][c.z] = c.warning;
				break;
			}
		}
	}

	void Check(const GameStateBlob& prevState,
		const GameStateBlob& currentState,
		std::vector<DeltaStateBlob>& outDeltas) override
	{
		const AsteroidShooterGameState* prevGS = reinterpret_cast<const AsteroidShooterGameState*>(prevState.data);
		const AsteroidShooterGameState* currGS = reinterpret_cast<const AsteroidShooterGameState*>(currentState.data);

		ticksSinceFullSnapshot++;
		if (ticksSinceFullSnapshot >= kFullSnapshotIntervalTicks)
		{
			ticksSinceFullSnapshot = 0;
			DeltaStateBlob deltaBlob;
			deltaBlob.delta_type = DELTA_WALL_STATE;
			WriteFullSnapshot(deltaBlob, currGS);
			outDeltas.push_back(deltaBlob);
			return;
		}

		WallEdgeChange changes[WALL_DELTA_MAX_SPARSE_CHANGES];
		int changeCount = 0;
		bool overflowed = false;

		auto note = [&](WallEdgeKind kind, int x, int y, int z, bool enabled, bool warning)
		{
			if (changeCount >= WALL_DELTA_MAX_SPARSE_CHANGES) { overflowed = true; return; }
			changes[changeCount++] = { kind, (uint8_t)x, (uint8_t)y, (uint8_t)z, enabled, warning };
		};

		for (int x = 0; x < MAP_SIZE; x++)
			for (int y = 0; y < MAP_SIZE; y++)
				if (prevGS->tilesActive[x][y] != currGS->tilesActive[x][y] ||
					prevGS->tilesWarning[x][y] != currGS->tilesWarning[x][y])
					note(WallEdgeKind::Tile, x, y, 0, currGS->tilesActive[x][y], currGS->tilesWarning[x][y]);

		for (int x = 0; x < 2 * MAP_SIZE + 1; x++)
			for (int y = 0; y < 2 * MAP_SIZE; y++)
				if (prevGS->hWalls[x][y] != currGS->hWalls[x][y] ||
					prevGS->hWallsWarning[x][y] != currGS->hWallsWarning[x][y])
					note(WallEdgeKind::HWall, x, y, 0, currGS->hWalls[x][y], currGS->hWallsWarning[x][y]);

		for (int x = 0; x < 2 * MAP_SIZE; x++)
			for (int y = 0; y < 2 * MAP_SIZE + 1; y++)
				if (prevGS->vWalls[x][y] != currGS->vWalls[x][y] ||
					prevGS->vWallsWarning[x][y] != currGS->vWallsWarning[x][y])
					note(WallEdgeKind::VWall, x, y, 0, currGS->vWalls[x][y], currGS->vWallsWarning[x][y]);

		for (int x = 0; x < MAP_SIZE; x++)
			for (int y = 0; y < MAP_SIZE; y++)
				for (int z = 0; z < 4; z++)
					if (prevGS->cWalls[x][y][z] != currGS->cWalls[x][y][z] ||
						prevGS->cWallsWarning[x][y][z] != currGS->cWallsWarning[x][y][z])
						note(WallEdgeKind::CWall, x, y, z, currGS->cWalls[x][y][z], currGS->cWallsWarning[x][y][z]);

		DeltaStateBlob deltaBlob;
		deltaBlob.delta_type = DELTA_WALL_STATE;

		if (overflowed)
		{
			// More edges changed this tick than the sparse budget covers
			// (e.g. a tile died and dragged its neighbours' border walls
			// with it) — a full snapshot is simpler and safer than growing
			// the sparse list further.
			WriteFullSnapshot(deltaBlob, currGS);
			ticksSinceFullSnapshot = 0;
			outDeltas.push_back(deltaBlob);
		}
		else if (changeCount > 0)
		{
			deltaBlob.data[0] = 1;
			deltaBlob.data[1] = (uint8_t)changeCount;
			std::memcpy(deltaBlob.data + 2, changes, changeCount * sizeof(WallEdgeChange));
			deltaBlob.len = 2 + (int)(changeCount * sizeof(WallEdgeChange));
			outDeltas.push_back(deltaBlob);
		}
		// else: nothing changed — the common case — send nothing this tick.
	}

	bool Compare(const DeltaStateBlob& delta,
		const GameStateBlob& currentState) override
	{
		const AsteroidShooterGameState* gs = reinterpret_cast<const AsteroidShooterGameState*>(currentState.data);
		if (delta.len < 1) return true; // malformed: don't force a correction over garbage

		if (delta.data[0] == 0)
		{
			if ((size_t)delta.len < 1 + sizeof(WallStateDelta)) return true;
			const WallStateDelta* wsd = reinterpret_cast<const WallStateDelta*>(delta.data + 1);

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

		if (delta.len < 2) return true;
		uint8_t count = delta.data[1];
		size_t needed = 2 + (size_t)count * sizeof(WallEdgeChange);
		if ((size_t)delta.len < needed) return true;

		// Sparse: only the listed edges are checked. Anything not listed
		// wasn't touched by the server this tick, so the client's existing
		// (already-synced) copy is trusted to still be correct — that's the
		// whole point of a diff instead of a full resend every time.
		const WallEdgeChange* changes = reinterpret_cast<const WallEdgeChange*>(delta.data + 2);
		for (uint8_t i = 0; i < count; i++)
		{
			const WallEdgeChange& c = changes[i];
			switch (c.kind)
			{
			case WallEdgeKind::Tile:
				if (gs->tilesActive[c.x][c.y] != c.enabled || gs->tilesWarning[c.x][c.y] != c.warning) return false;
				break;
			case WallEdgeKind::HWall:
				if (gs->hWalls[c.x][c.y] != c.enabled || gs->hWallsWarning[c.x][c.y] != c.warning) return false;
				break;
			case WallEdgeKind::VWall:
				if (gs->vWalls[c.x][c.y] != c.enabled || gs->vWallsWarning[c.x][c.y] != c.warning) return false;
				break;
			case WallEdgeKind::CWall:
				if (gs->cWalls[c.x][c.y][c.z] != c.enabled || gs->cWallsWarning[c.x][c.y][c.z] != c.warning) return false;
				break;
			}
		}
		return true;
	}
};