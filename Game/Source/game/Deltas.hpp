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

    // Piggybacked here (not its own delta type) because this handler
    // already sends every tick unconditionally — exactly the cadence the
    // countdown needs for a smooth display and for the client's local
    // prediction to unblock input in step with the server.
    int startCountdownTicks;
};

// Full boolean-grid snapshot — the DELTA_WALL_STATE wire format's "mode 0"
// payload (see WallStateDeltaHandler). Used only as a periodic keyframe and
// as a fallback when a single tick changes more edges than the sparse
// format's budget, not sent every tick anymore.
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

// DELTA_WALL_STATE "mode 1" payload: one entry per grid cell whose
// enabled/warning actually changed since the previous tick. `x`/`y` index
// the relevant grid (tilesActive, hWalls, vWalls or cWalls, depending on
// `kind`); `z` is only meaningful for CWall (spoke direction, 0-3).
enum class WallEdgeKind : uint8_t { Tile, HWall, VWall, CWall };

struct WallEdgeChange {
    WallEdgeKind kind;
    uint8_t x, y, z;
    bool enabled;
    bool warning;
};

// Sparse changes rarely exceed a handful per tick (walls re-roll a timer of
// 3-30s before toggling again); the worst realistic burst is a tile
// destruction dragging its neighbours' border walls with it. 64 comfortably
// covers that with room to spare, and DeltaStateBlob::data is 1024 bytes
// wide — 2 header bytes + 64 * sizeof(WallEdgeChange) fits easily.
constexpr int WALL_DELTA_MAX_SPARSE_CHANGES = 64;
