#pragma once

#include <cstdint>

const int NUM_PLAYERS = 12;
const int MAX_BULLETS = 32;
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

    float velX[NUM_PLAYERS];
    float velY[NUM_PLAYERS];
    float angularVel[NUM_PLAYERS];

    int health[NUM_PLAYERS];
    bool alive[NUM_PLAYERS];

    bool isMovingForward[NUM_PLAYERS];

    int shipInclination[NUM_PLAYERS];

    bool isShooting[NUM_PLAYERS];
    int remaingShootFrames[NUM_PLAYERS];

    int shootCooldown[NUM_PLAYERS];

    Bullet bullets[MAX_BULLETS];
    int bulletCount;  // Number of active bullets (for quick iteration)

    bool tilesActive[MAP_SIZE][MAP_SIZE];
    bool tilesWarning[MAP_SIZE][MAP_SIZE];

    // Edge between (px,py)→(px,py+1): runs along X axis (horizontal wall)
    bool hWalls[2 * MAP_SIZE + 1][2 * MAP_SIZE];
    // Edge between (px,py)→(px+1,py): runs along Y axis (vertical wall)
    bool vWalls[2 * MAP_SIZE][2 * MAP_SIZE + 1];
    // Center spokes per cell: 0=Down, 1=Up, 2=Left, 3=Right
    bool cWalls[MAP_SIZE][MAP_SIZE][4];

    // Wall warning codes (see EncodeWallWarning): 0 = none, else warning, and for an off wall about to energise also
    // the state frame it turns on in, so the client can show it on in its own predicted timeline.
    uint16_t hWallsWarning[2 * MAP_SIZE + 1][2 * MAP_SIZE];
    uint16_t vWallsWarning[2 * MAP_SIZE][2 * MAP_SIZE + 1];
    uint16_t cWallsWarning[MAP_SIZE][MAP_SIZE][4];

    // Pre-match freeze countdown, in ticks; see MatchStartTimer/MatchStartSystem.
    int startCountdownTicks;
};

// Must fit GameStateBlob::data (4096 bytes).
static_assert(sizeof(AsteroidShooterGameState) <= 4096, "AsteroidShooterGameState no longer fits in a GameStateBlob");

// The local ship is drawn framesAheadOfServer ticks ahead of the last server state, but walls came from that server
// state, so a wall appeared on at least that many ticks late relative to the ship: a ship seen already past a warning
// wall could still be inside it on the server when it energised, and die "to nothing". The server knows 3 s ahead
// (the warning window) the state frame a wall turns on in, and sends it in the warning code; the client draws the wall
// on as soon as its predicted frame reaches it (AsteroidShooterGameRenderer::Interpolate).
//
// Code: 0 = no warning; 1 = warning without a known on-frame (e.g. an on wall about to turn off); >= 2 = warning, and
// the wall turns on in state frame F with (F % WALL_WARN_FRAME_MOD) == code - 2. uint16 keeps the full wall keyframe
// inside the 1024-byte delta blob; the modulo is resolved against any nearby frame (the on-frame is <= ~92 ticks away).
constexpr uint16_t WALL_WARN_NONE = 0;
constexpr uint16_t WALL_WARN_NO_FRAME = 1;
constexpr int      WALL_WARN_FRAME_MOD = 65000;

inline uint16_t EncodeWallWarning(bool warning, int onFrame)
{
    if (!warning) return WALL_WARN_NONE;
    if (onFrame < 0) return WALL_WARN_NO_FRAME;
    return static_cast<uint16_t>(2 + onFrame % WALL_WARN_FRAME_MOD);
}

// On-frame carried by a code, resolved to the frame nearest refFrame; -1 when the code carries none.
inline int DecodeWallOnFrame(uint16_t code, int refFrame)
{
    if (code < 2) return -1;
    const int r = code - 2;
    int frame = refFrame - (refFrame % WALL_WARN_FRAME_MOD) + r;
    if (frame < refFrame - WALL_WARN_FRAME_MOD / 2) frame += WALL_WARN_FRAME_MOD;
    else if (frame > refFrame + WALL_WARN_FRAME_MOD / 2) frame -= WALL_WARN_FRAME_MOD;
    return frame;
}