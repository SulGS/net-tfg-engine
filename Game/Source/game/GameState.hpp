#pragma once

// Constants
const int MAX_BULLETS = 32;  // Adjust based on your needs

struct Bullet {
    int id;
    float posX;
    float posY;
    float velX;
    float velY;
    int ownerId;    // Which player shot it
    bool active;    // Is this bullet slot in use?
    int lifetime;   // Frames remaining (for cleanup)
};

struct AsteroidShooterGameState {
    float posX[2];
    float posY[2];
    int health[2];
	bool alive[2];
    float rot[2];

    // Bullet pool
    Bullet bullets[MAX_BULLETS];
    int bulletCount;  // Number of active bullets (for quick iteration)

    // Shooting cooldown per player
    int shootCooldown[2];
    int deathCooldown[2];
};