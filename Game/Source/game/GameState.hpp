#pragma once

// Constants
const int MAX_BULLETS = 32;  // Adjust based on your needs
const int MAP_SIZE = 5;      // 5x5 grid of tiles

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
    float rot[2];

    int health[2];
	bool alive[2];

	bool isMovingForward[2];

	int shipInclination[2];
    

	bool isShooting[2];
    int remaingShootFrames[2];

    // Shooting cooldown per player
    int shootCooldown[2];
    int deathCooldown[2];

    // Bullet pool
    Bullet bullets[MAX_BULLETS];
    int bulletCount;  // Number of active bullets (for quick iteration)

	// State of arena
    bool tilesActive[MAP_SIZE][MAP_SIZE];
    // bool matrix for walls

    // Edge between (px,py)→(px,py+1): runs along X axis (horizontal wall)
    bool hWalls[2 * MAP_SIZE + 1][2 * MAP_SIZE];
    // Edge between (px,py)→(px+1,py): runs along Y axis (vertical wall)
    bool vWalls[2 * MAP_SIZE][2 * MAP_SIZE + 1];
    // Center spokes per cell: 0=Down, 1=Up, 2=Left, 3=Right
    bool cWalls[MAP_SIZE][MAP_SIZE][4];

    bool hWallsWarning[2 * MAP_SIZE + 1][2 * MAP_SIZE];
    bool vWallsWarning[2 * MAP_SIZE][2 * MAP_SIZE + 1];
    bool cWallsWarning[MAP_SIZE][MAP_SIZE][4];
};