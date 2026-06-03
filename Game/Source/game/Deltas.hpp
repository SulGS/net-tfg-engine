#pragma once

enum AsteroidsDeltaTypes {
    DELTA_GAME_POSITIONS = 0
};

struct GamePositionsDelta {
    float posX[NUM_PLAYERS];
    float posY[NUM_PLAYERS];
    float rot[NUM_PLAYERS];
};