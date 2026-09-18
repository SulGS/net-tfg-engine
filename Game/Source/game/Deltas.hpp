#pragma once

enum AsteroidsDeltaTypes {
    DELTA_GAME_POSITIONS = 0,
    DELTA_WALL_STATE = 1
};

struct GamePositionsDelta {
    float posX[NUM_PLAYERS];
    float posY[NUM_PLAYERS];
    float rot[NUM_PLAYERS];
};

struct WallStateDelta {
    bool tilesActive[MAP_SIZE][MAP_SIZE];
    bool tilesWarning[MAP_SIZE][MAP_SIZE];

    bool hWalls[2 * MAP_SIZE + 1][2 * MAP_SIZE];
    bool vWalls[2 * MAP_SIZE][2 * MAP_SIZE + 1];
    bool cWalls[MAP_SIZE][MAP_SIZE][4];

    bool hWallsWarning[2 * MAP_SIZE + 1][2 * MAP_SIZE];
    bool vWallsWarning[2 * MAP_SIZE][2 * MAP_SIZE + 1];
    bool cWallsWarning[MAP_SIZE][MAP_SIZE][4];
};
