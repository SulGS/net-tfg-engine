#ifndef COMPONENTS_ASTEROIDS
#define COMPONENTS_ASTEROIDS

#include "ecs/ecs_common.hpp"


class SpaceShip : public IComponent {
public:
    int health;
    int remainingShootFrames;
    int shootCooldown; // frames until can shoot again
    int deathCooldown;
    bool isAlive;

    SpaceShip() : health(100), shootCooldown(0), deathCooldown(0), isAlive(true) {}
    SpaceShip(int h, int rsf, int cd, int dc, bool al) : health(h), remainingShootFrames(rsf), shootCooldown(cd), deathCooldown(dc), isAlive(al) {}
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

#endif // COMPONENTS_ASTEROIDS