#ifndef COMPONENTS_ASTEROIDS
#define COMPONENTS_ASTEROIDS

#include "ecs/ecs_common.hpp"


class SpaceShip : public IComponent {
public:
    int health;
    int shootCooldown; // frames until can shoot again
    int deathCooldown;

    SpaceShip() : health(100), shootCooldown(0), deathCooldown(0) {}
    SpaceShip(int h, int cd, int dc) : health(h), shootCooldown(cd), deathCooldown(dc) {}
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

#endif // COMPONENTS_ASTEROIDS