#ifndef COMPONENTS_ASTEROIDS
#define COMPONENTS_ASTEROIDS

#include "ecs/ecs_common.hpp"

enum class CellCardinalDirection {
	None,
	Up,
	Right,
	Down,
	Left
};

struct CenterSpoke : public IComponent {
	// Marker component for walls that go from cell center to edge midpoint
};

// --- Laser walls ---
		// Each wall is owned by one cell and faces one direction.
		// Right + Up walls are emitted for all cells (covering all shared interior
		// edges), plus Left for column 0 and Down for row 0 (the remaining borders).
		// Border walls start enabled; interior walls start disabled.
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
	bool warning;  // true when timer is close to 0
	LaserWallID() : cellId(-1), dir(CellCardinalDirection::None), enabled(true), timer(0.0f), warning(false) {}
	LaserWallID(int c, CellCardinalDirection d) : cellId(c), dir(d), enabled(true), timer(0.0f), warning(false) {}
};

class PillarID : public IComponent {
public:
	std::vector<int> cellsIsIn; // List of cell IDs this pillar is part of (max 4)
	PillarID(){}
	void addCell(int cellId) {
		if (cellsIsIn.size() < 4) {
			cellsIsIn.push_back(cellId);
		}
	}
};

class TileID : public IComponent {
public:
	int id;
	TileID() : id(0) {}
	TileID(int id) : id(id) {}
};

class ThrusterOwner : public IComponent {
public:
	int shipEntity; // The ship this thruster belongs to
	bool isLeftEngine; // True if left thruster, false if right
    bool isSmoke = false;
	ThrusterOwner() : shipEntity(-1), isSmoke(false), isLeftEngine(false){}
	ThrusterOwner(int se, bool isSm, bool isLeftE) : shipEntity(se), isSmoke(isSm), isLeftEngine(isLeftE) {}
};

class SpaceShip : public IComponent {
public:
    int health;
	bool isShooting;
    bool isMovingForward;
	int shipInclination;

    int remainingShootFrames;
    int shootCooldown; // frames until can shoot again
    int deathCooldown;
    bool isAlive;

	int shipZRotation;

    SpaceShip() : health(100), isShooting(false), remainingShootFrames(0), shootCooldown(0), deathCooldown(0), isAlive(true), isMovingForward(false), shipInclination(0), shipZRotation(0) {}
    SpaceShip(int h, int rsf, int cd, int dc, bool al) : health(h), isShooting(false), remainingShootFrames(rsf), shootCooldown(cd), deathCooldown(dc), isAlive(al), isMovingForward(false), shipInclination(0), shipZRotation(0) {}
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

#endif // COMPONENTS_ASTEROIDS