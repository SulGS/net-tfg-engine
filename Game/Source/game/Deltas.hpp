#pragma once

#include <cstdint>

enum AsteroidsDeltaTypes {
    DELTA_GAME_POSITIONS = 0,
    DELTA_WALL_STATE = 1
};

struct GamePositionsDelta {
    float posX[NUM_PLAYERS];
    float posY[NUM_PLAYERS];
    float rot[NUM_PLAYERS];

    // Piggybacked here because this handler already sends every tick — exactly the cadence the countdown
    // needs for smooth display and for client prediction to unblock input in step with the server.
    int startCountdownTicks;
};

// Full boolean-grid snapshot: DELTA_WALL_STATE "mode 0" payload (see WallStateDeltaHandler). Only sent as a
// periodic keyframe or when a tick changes more edges than the sparse budget.
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

// DELTA_WALL_STATE "mode 1" payload: one entry per cell whose enabled/warning changed since the previous tick.
// `x`/`y` index the grid chosen by `kind` (tilesActive, hWalls, vWalls, cWalls); `z` is only for CWall (spoke dir 0-3).
enum class WallEdgeKind : uint8_t { Tile, HWall, VWall, CWall };

struct WallEdgeChange {
    WallEdgeKind kind;
    uint8_t x, y, z;
    bool enabled;
    bool warning;
};

// Sparse changes rarely exceed a handful per tick (walls re-roll a 3-30s timer); worst case is a tile destruction
// dragging its border walls. 64 covers that with room to spare: 2 + 64 * sizeof(WallEdgeChange) fits in the 1024-byte blob.
constexpr int WALL_DELTA_MAX_SPARSE_CHANGES = 64;
