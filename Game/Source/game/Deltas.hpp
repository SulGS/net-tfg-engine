#pragma once

enum AsteroidsDeltaTypes {
    DELTA_GAME_POSITIONS = 0
};

struct GamePositionsDelta {
    float posX[2];
    float posY[2];
    float rot[2];
};