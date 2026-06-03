#pragma once

// Constants
const int NUM_PLAYERS = 3;
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
    float posX[NUM_PLAYERS];
    float posY[NUM_PLAYERS];
    float rot[NUM_PLAYERS];

    float velX[NUM_PLAYERS];        // ← new
    float velY[NUM_PLAYERS];        // ← new
    float angularVel[NUM_PLAYERS];  // ← new

    int health[NUM_PLAYERS];
    bool alive[NUM_PLAYERS];

    bool isMovingForward[NUM_PLAYERS];

    int shipInclination[NUM_PLAYERS];

    bool isShooting[NUM_PLAYERS];
    int remaingShootFrames[NUM_PLAYERS];

    // Shooting cooldown per player
    int shootCooldown[NUM_PLAYERS];

    // Bullet pool
    Bullet bullets[MAX_BULLETS];
    int bulletCount;  // Number of active bullets (for quick iteration)

    // State of arena
    bool tilesActive[MAP_SIZE][MAP_SIZE];
    bool tilesWarning[MAP_SIZE][MAP_SIZE];
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