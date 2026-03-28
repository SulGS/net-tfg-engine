#ifndef COMPONENTS_ASTEROIDS
#define COMPONENTS_ASTEROIDS

#include "ecs/ecs_common.hpp"

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