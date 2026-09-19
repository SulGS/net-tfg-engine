#ifndef COMPONENTS_ASTEROIDS
#define COMPONENTS_ASTEROIDS

#include "ecs/ecs_common.hpp"
#include "GameState.hpp"
#include <unordered_set>

enum class CellCardinalDirection {
	None,
	Up,
	Right,
	Down,
	Left
};

inline CellCardinalDirection OppositeDirection(CellCardinalDirection dir)
{
	switch (dir)
	{
	case CellCardinalDirection::Left:  return CellCardinalDirection::Right;
	case CellCardinalDirection::Right: return CellCardinalDirection::Left;
	case CellCardinalDirection::Down:  return CellCardinalDirection::Up;
	case CellCardinalDirection::Up:    return CellCardinalDirection::Down;
	default: return CellCardinalDirection::None;
	}
}

// Returns the neighbouring cell id sharing an edge with `cellId` in `dir`,
// or -1 if that neighbour would fall outside the MAP_SIZE x MAP_SIZE grid.
inline int NeighborCellId(int cellId, CellCardinalDirection dir)
{
	int cx = cellId / MAP_SIZE;
	int cy = cellId % MAP_SIZE;

	int nx = cx, ny = cy;
	switch (dir)
	{
	case CellCardinalDirection::Left:  nx = cx - 1; break;
	case CellCardinalDirection::Right: nx = cx + 1; break;
	case CellCardinalDirection::Down:  ny = cy - 1; break;
	case CellCardinalDirection::Up:    ny = cy + 1; break;
	default: break;
	}

	if (nx < 0 || nx >= MAP_SIZE || ny < 0 || ny >= MAP_SIZE)
		return -1;

	return nx * MAP_SIZE + ny;
}

// A shared interior edge is now represented by exactly ONE LaserWallID
// entity (see the wall-building loops in asteroids.hpp), owned by whichever
// of its two cells happened to be visited first. Since either side can die
// independently (a tile getting destroyed), classifying "is this wall solid"
// has to look at BOTH the entity's own stored cell and its geometric
// neighbour, not just the one it happens to be stored under:
//   - both cells alive       -> Interior: normal random on/off toggling.
//   - exactly one cell alive -> SoleBorder: forced solid, protects the
//     survivor from falling into the void where the other cell used to be.
//   - neither cell alive (or this is a map-edge wall whose only cell died)
//     -> Dead: forced off, hidden, no collider.
enum class WallEdgeState { Interior, SoleBorder, Dead };

inline WallEdgeState ClassifyWallEdge(int cellId, CellCardinalDirection dir,
	const std::unordered_set<int>& activeCellIds)
{
	bool selfActive = activeCellIds.count(cellId) > 0;
	int neighborId = NeighborCellId(cellId, dir);

	if (neighborId == -1)
		return selfActive ? WallEdgeState::SoleBorder : WallEdgeState::Dead;

	bool neighborActive = activeCellIds.count(neighborId) > 0;
	if (selfActive && neighborActive) return WallEdgeState::Interior;
	if (selfActive != neighborActive) return WallEdgeState::SoleBorder;
	return WallEdgeState::Dead;
}

struct CenterSpoke : public IComponent {
	// Marker component for walls that go from cell center to edge midpoint
};

// Singleton (one entity, both worlds): counts down the pre-match freeze —
// see MatchStartSystem in LogicSystems.hpp. Synced through
// AsteroidShooterGameState::startCountdownTicks (piggybacked on
// GamePositionsDelta, which is already sent every tick) so the client's
// local prediction blocks input in lockstep with the server instead of
// drifting during the freeze.
class MatchStartTimer : public IComponent {
public:
	int ticksRemaining;
	MatchStartTimer() : ticksRemaining(0) {}
	MatchStartTimer(int t) : ticksRemaining(t) {}
};

// Laser walls: each shared edge is ONE entity, stored from one of its two
// bordering cells' point of view (see ClassifyWallEdge above for why both
// sides still matter); border walls start enabled, interior ones disabled.
struct WallDef
{
	int                   cellX, cellY;
	CellCardinalDirection dir;
	glm::vec3             pos;
	glm::vec3             rot;
	bool                  onBorder;
};

class LaserWallID : public IComponent {
public:
	int cellId;
	CellCardinalDirection dir;
	float timer;
	bool enabled;
	bool warning;
	LaserWallID() : cellId(-1), dir(CellCardinalDirection::None), enabled(true), timer(0.0f), warning(false) {}
	LaserWallID(int c, CellCardinalDirection d) : cellId(c), dir(d), enabled(true), timer(0.0f), warning(false) {}
};

class PillarID : public IComponent {
public:
	std::vector<int> cellsIsIn; // List of cell IDs this pillar is part of (max 4)
	PillarID() {}
	void addCell(int cellId) {
		if (cellsIsIn.size() < 4) {
			cellsIsIn.push_back(cellId);
		}
	}
};

class TileID : public IComponent {
public:
	int id;
	bool warning = false;
	bool active = true;
	float warningFallAccum = 0.0f; // renderer-only: accumulates fall distance during warning
	TileID() : id(0), warning(false), active(true), warningFallAccum(0.0f) {}
	TileID(int id) : id(id), warning(false), active(true), warningFallAccum(0.0f) {}
};

class ThrusterOwner : public IComponent {
public:
	int shipEntity;
	bool isLeftEngine;
	bool isSmoke = false;
	ThrusterOwner() : shipEntity(-1), isSmoke(false), isLeftEngine(false) {}
	ThrusterOwner(int se, bool isSm, bool isLeftE) : shipEntity(se), isSmoke(isSm), isLeftEngine(isLeftE) {}
};

class SpaceShip : public IComponent {
public:
	int health;
	bool isShooting;
	bool isMovingForward;
	int shipInclination;
	float velX;
	float velY;
	float angularVel;

	int remainingShootFrames;
	int shootCooldown; // frames until can shoot again
	bool isAlive;

	int shipZRotation;

	SpaceShip() : health(1), isShooting(false), remainingShootFrames(0), shootCooldown(0), isAlive(true), isMovingForward(false), shipInclination(0), shipZRotation(0), velX(0.0f), velY(0.0f), angularVel(0.0f) {}
	SpaceShip(int h, int rsf, int cd, bool al) : health(h), isShooting(false), remainingShootFrames(rsf), shootCooldown(cd), isAlive(al), isMovingForward(false), shipInclination(0), shipZRotation(0), velX(0.0f), velY(0.0f), angularVel(0.0f) {}
};

// Tracks which player a dead local player is currently spectating.
// Only present on the local player's entity (renderer side).
class SpectatorState : public IComponent {
public:
	int watchedPlayerId;
	bool prevLeftHeld;   // edge-detection for left arrow
	bool prevRightHeld;  // edge-detection for right arrow

	SpectatorState() : watchedPlayerId(0), prevLeftHeld(false), prevRightHeld(false) {}
};

class ECSBullet : public IComponent {
public:
	int id;
	float velX;
	float velY;
	int ownerId;    // Which player shot it
	int lifetime;   // Frames remaining (for cleanup)

	ECSBullet() : id(-1), velX(0), velY(0), ownerId(-1), lifetime(0) {}
	ECSBullet(int i, float vx, float vy, int oid, int lt) : id(i), velX(vx), velY(vy), ownerId(oid), lifetime(lt) {}
};

class ChargingShootEffect : public IComponent {
public:
	int entity;
	ChargingShootEffect() : entity(0) {}
};

class DestroyTimer : public IComponent {
public:
	int framesRemaining;
	DestroyTimer() : framesRemaining(0) {}
	DestroyTimer(int fr) : framesRemaining(fr) {}
};

class JustDeathChecker : public IComponent {
public:
	bool notExecuted;
	JustDeathChecker() : notExecuted(true) {}
};

class ExplosionPlayerID : public IComponent {
public:
	int playerId; // Which player's explosion sound to play
	ExplosionPlayerID() : playerId(-1) {}
	ExplosionPlayerID(int pid) : playerId(pid) {}
};

class LinkAudioToBullet : public IComponent {
public:
	int bulletId;
	LinkAudioToBullet() : bulletId(-1) {}
	LinkAudioToBullet(int bid) : bulletId(bid) {}
};

class ExitButtonChecker : public IComponent {
public:
	bool exitPressed;
	ExitButtonChecker() : exitPressed(false) {}
};

class ThrusterSound : public IComponent {
public:
	int shipEntity;
	ThrusterSound() : shipEntity(-1) {}
	ThrusterSound(int se) : shipEntity(se) {}
};

// Tag: marks a mesh entity whose Material needs "uTime" refreshed every
// render frame (fluid.vert/water.frag/lava.frag animation). See
// FluidAnimationSystem in RenderSystems.hpp.
class FluidSurface : public IComponent {
public:
};

// Tag: marks the game's own status label (health/"REMAINING"/"YOU DIED"/
// winner text) so CreateQuery<UIElement, UIText>() in RenderSystems.hpp
// only ever matches that one entity. Without it, those queries also pick up
// any other UIText in the same EntityManager — e.g. the DebugOverlay FPS/
// latency label that IECSGameRenderer::Init() adds to every scene — and
// reposition/overwrite it along with the real HUD text.
class GameStatusText : public IComponent {
public:
};

#endif // COMPONENTS_ASTEROIDS