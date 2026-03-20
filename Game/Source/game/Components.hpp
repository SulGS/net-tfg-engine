#ifndef COMPONENTS_ASTEROIDS
#define COMPONENTS_ASTEROIDS

#include "ecs/ecs_common.hpp"

class ThrusterOwner : public IComponent {
public:
	int shipEntity; // The ship this thruster belongs to
	ThrusterOwner() : shipEntity(-1) {}
	ThrusterOwner(int se) : shipEntity(se) {}
};

class SpaceShip : public IComponent {
public:
    int health;
	bool isShooting;
	bool isMoving;
    int remainingShootFrames;
    int shootCooldown; // frames until can shoot again
    int deathCooldown;
    bool isAlive;

    SpaceShip() : health(100), isShooting(false), remainingShootFrames(0), shootCooldown(0), deathCooldown(0), isAlive(true), isMoving(false) {}
    SpaceShip(int h, int rsf, int cd, int dc, bool al) : health(h), isShooting(false), remainingShootFrames(rsf), shootCooldown(cd), deathCooldown(dc), isAlive(al), isMoving(false) {}
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