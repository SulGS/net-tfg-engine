#pragma once

#include "netcode/netcode_common.hpp"
#include "ecs/ecs_common.hpp"
#include "Components.hpp"
#include "GameState.hpp"
#include "Utils/Debug/Debug.hpp"
#include "ecs/Collisions/BoxCollider2D.hpp"
#include <math.h>
#include <cmath>
#include <random>
#include <queue>
#include <unordered_set>
#include "Events.hpp"

enum InputMask : uint8_t {
    INPUT_NONE = 0,
    INPUT_LEFT = 1 << 0,
    INPUT_RIGHT = 1 << 1,
    INPUT_TOP = 1 << 2,
    INPUT_DOWN = 1 << 3,
    INPUT_SHOOT = 1 << 4
};

class BulletSystem : public ISystem {
public:
    void Update(EntityManager& entityManager, std::vector<EventEntry>& events, bool isServer, float deltaTime) override {
        const float WORLD_SIZE = 400.0f;
        auto query = entityManager.CreateQuery<Transform, ECSBullet>();
        for (auto [entity, transform, ecsb] : query) {
            transform->translate(glm::vec3(ecsb->velX, ecsb->velY, 0.0f));
            ecsb->lifetime--;
            if (ecsb->lifetime <= 0 ||
                transform->getPosition().x < -WORLD_SIZE || transform->getPosition().x > WORLD_SIZE ||
                transform->getPosition().y < -WORLD_SIZE || transform->getPosition().y > WORLD_SIZE) {
                entityManager.DestroyEntity(entity);
            }
        }
    }
};

class InputSystem : public ISystem {
public:
    void Update(EntityManager& entityManager, std::vector<EventEntry>& events, bool isServer, float deltaTime) override {
        auto query = entityManager.CreateQuery<Transform, Playable, SpaceShip>();
        for (auto [entity, transform, play, ship] : query) {
            int p = play->playerId;
            InputBlob input = play->input;
            uint8_t m = input.data[0];

            const float MOVE_SPEED = 1.0f;
            const float ROT_SPEED = 5.0f;
            const int CHARGE_SHOOT_FRAMES = 5;
            const int SHOOT_COOLDOWN = 10;

            float radians = transform->getRotation().z * 3.14159f / 180.0f;

            if (!ship->isAlive) continue;

            if (!isServer) {
                if (ship->shootCooldown > 0) ship->shootCooldown--;
                if (ship->remainingShootFrames > 0) ship->remainingShootFrames--;
                if (ship->remainingShootFrames == 0) ship->isShooting = false;
            }

            if (ship->isShooting) continue;

            bool notRotating = !(m & INPUT_LEFT) && !(m & INPUT_RIGHT);

            if (m & INPUT_LEFT) {
                transform->setRotation(transform->getRotation() + glm::vec3(0.0f, 0.0f, ROT_SPEED));
                if (transform->getRotation().z >= 360) transform->setRotation(transform->getRotation() + glm::vec3(0.0f, 0.0f, -360.0f));
                ship->shipInclination = std::min(ship->shipInclination + 5, 40);
            }
            if (m & INPUT_RIGHT) {
                transform->setRotation(transform->getRotation() + glm::vec3(0.0f, 0.0f, -ROT_SPEED));
                if (transform->getRotation().z < 0) transform->setRotation(transform->getRotation() + glm::vec3(0.0f, 0.0f, 360.0f));
                ship->shipInclination = std::max(ship->shipInclination - 5, -40);
            }
            if (notRotating) {
                if (ship->shipInclination > 0) ship->shipInclination = std::max(ship->shipInclination - 3, 0);
                else if (ship->shipInclination < 0) ship->shipInclination = std::min(ship->shipInclination + 3, 0);
            }

            float velX = 0, velY = 0;
            if (m & INPUT_TOP) {
                velX += cos(radians) * MOVE_SPEED;
                velY += sin(radians) * MOVE_SPEED;
                ship->isMovingForward = true;
            }
            else {
                ship->isMovingForward = false;
            }
            if (m & INPUT_DOWN) {
                velX -= cos(radians) * MOVE_SPEED;
                velY -= sin(radians) * MOVE_SPEED;
            }

            transform->setPosition(transform->getPosition() + glm::vec3(velX, velY, 0.0f));

            if ((m & INPUT_SHOOT) && ship->shootCooldown <= 0 && ship->isAlive) {
                ship->remainingShootFrames = CHARGE_SHOOT_FRAMES;
                ship->isShooting = true;
            }
        }
    }
};

class ArenaSystem : public ISystem {
private:
    std::mt19937 rng{ std::random_device{}() };
    const int x_size = MAP_SIZE;
    const int y_size = MAP_SIZE;

    const float WARNING_THRESHOLD = 3.0f;
    const float TILE_DESTROY_INTERVAL = 30.0f;
    const float TILE_WARNING_THRESHOLD = 3.0f;

    float debugPrintArenaTimer = 10.0f;
    float tileDestroyTimer = TILE_DESTROY_INTERVAL;
    int   pendingDestroyTileId = -1;
    bool  tileWarningEmitted = false;

    float RandomTimer()
    {
        std::uniform_real_distribution<float> dist(3.0f, 30.0f);
        return dist(rng);
    }

    bool IsBorderWall(int cellId, CellCardinalDirection dir,
        const std::vector<std::pair<int, int>>& activeTiles)
    {
        int cx = cellId / y_size;
        int cy = cellId % y_size;

        int nx = cx, ny = cy;
        switch (dir)
        {
        case CellCardinalDirection::Left:  nx = cx - 1; break;
        case CellCardinalDirection::Right: nx = cx + 1; break;
        case CellCardinalDirection::Down:  ny = cy - 1; break;
        case CellCardinalDirection::Up:    ny = cy + 1; break;
        default: break;
        }

        if (nx < 0 || nx >= x_size || ny < 0 || ny >= y_size)
            return true;

        for (auto& t : activeTiles)
            if (t.first == nx && t.second == ny)
                return false;

        return true;
    }

    void BuildWallMap(
        EntityManager& entityManager,
        const std::unordered_set<Entity>& spokeEntities,
        bool hWalls[2 * MAP_SIZE + 1][2 * MAP_SIZE],
        bool vWalls[2 * MAP_SIZE][2 * MAP_SIZE + 1],
        bool cWalls[MAP_SIZE][MAP_SIZE][4])
    {
        std::memset(hWalls, 0, sizeof(bool) * (2 * MAP_SIZE + 1) * (2 * MAP_SIZE));
        std::memset(vWalls, 0, sizeof(bool) * (2 * MAP_SIZE) * (2 * MAP_SIZE + 1));
        std::memset(cWalls, 0, sizeof(bool) * MAP_SIZE * MAP_SIZE * 4);

        std::unordered_set<int> activeTileSet;
        {
            auto tq = entityManager.CreateQuery<TileID>();
            for (auto [e, tid] : tq)
                if (tid->active) activeTileSet.insert(tid->id);
        }

        auto wallQuery = entityManager.CreateQuery<LaserWallID>();
        for (auto [entity, lwid] : wallQuery)
        {
            if (!lwid->enabled) continue;
            if (!activeTileSet.count(lwid->cellId)) continue;

            int cx = lwid->cellId / y_size;
            int cy = lwid->cellId % y_size;

            if (spokeEntities.count(entity))
            {
                switch (lwid->dir)
                {
                case CellCardinalDirection::Down:  cWalls[cx][cy][0] = true; break;
                case CellCardinalDirection::Up:    cWalls[cx][cy][1] = true; break;
                case CellCardinalDirection::Left:  cWalls[cx][cy][2] = true; break;
                case CellCardinalDirection::Right: cWalls[cx][cy][3] = true; break;
                default: break;
                }
            }
            else
            {
                switch (lwid->dir)
                {
                case CellCardinalDirection::Down:  vWalls[2 * cx][2 * cy] = true; break;
                case CellCardinalDirection::Up:    vWalls[2 * cx][2 * cy + 2] = true; break;
                case CellCardinalDirection::Left:  hWalls[2 * cx][2 * cy] = true; break;
                case CellCardinalDirection::Right: hWalls[2 * cx + 2][2 * cy] = true; break;
                default: break;
                }
            }
        }
    }

    // Subtile-level reachability flood-fill.
    // Each tile (cx,cy) has 4 subtiles: TL=0, TR=1, BL=2, BR=3
    //
    // Internal adjacency (within a tile, blocked by spokes):
    //   TL<->TR: vSpoke    TL<->BL: hSpoke
    //   TR<->BR: hSpoke    BL<->BR: vSpoke
    //
    // External adjacency (across tile edges, blocked by edge walls):
    //   TL(cx,cy) <-> TR(cx-1,cy): hWalls[2*cx][2*cy]
    //   TR(cx,cy) <-> TL(cx+1,cy): hWalls[2*cx+2][2*cy]
    //   BL(cx,cy) <-> BR(cx-1,cy): hWalls[2*cx][2*cy]
    //   BR(cx,cy) <-> BL(cx+1,cy): hWalls[2*cx+2][2*cy]
    //   TL(cx,cy) <-> BL(cx,cy+1): vWalls[2*cx][2*cy+2]
    //   TR(cx,cy) <-> BR(cx,cy+1): vWalls[2*cx][2*cy+2]
    //   BL(cx,cy) <-> TL(cx,cy-1): vWalls[2*cx][2*cy]
    //   BR(cx,cy) <-> TR(cx,cy-1): vWalls[2*cx][2*cy]
    bool IsMapValid(
        const std::vector<std::pair<int, int>>& activeTiles,
        const bool hWalls[2 * MAP_SIZE + 1][2 * MAP_SIZE],
        const bool vWalls[2 * MAP_SIZE][2 * MAP_SIZE + 1],
        const bool cWalls[MAP_SIZE][MAP_SIZE][4])
    {
        if (activeTiles.empty()) return true;

        bool tileExists[MAP_SIZE][MAP_SIZE] = {};
        for (auto& t : activeTiles)
            tileExists[t.first][t.second] = true;

        // visited[cx][cy][subtile]
        bool visited[MAP_SIZE][MAP_SIZE][4] = {};

        int total = (int)activeTiles.size() * 4;
        int count = 0;

        std::queue<std::tuple<int, int, int>> q;

        // Find the first subtile that has at least one open passage (internal or external).
        // Subtile 0 of tile 0 may be sealed by border walls + spokes, giving a false failure.
        bool foundStart = false;
        for (auto& [scx, scy] : activeTiles)
        {
            bool hSpoke = cWalls[scx][scy][0] || cWalls[scx][scy][1];
            bool vSpoke = cWalls[scx][scy][2] || cWalls[scx][scy][3];
            for (int sst = 0; sst < 4; sst++)
            {
                bool canReach = false;
                switch (sst)
                {
                case 0: // TL: left exit, up exit, right(vSpoke), down(hSpoke)
                    canReach = (!hWalls[2 * scx][2 * scy] && scx > 0 && tileExists[scx - 1][scy])
                        || (!vWalls[2 * scx][2 * scy + 2] && scy < y_size - 1 && tileExists[scx][scy + 1])
                        || !vSpoke || !hSpoke;
                    break;
                case 1: // TR: right exit, up exit, left(vSpoke), down(hSpoke)
                    canReach = (!hWalls[2 * scx + 2][2 * scy] && scx < x_size - 1 && tileExists[scx + 1][scy])
                        || (!vWalls[2 * scx][2 * scy + 2] && scy < y_size - 1 && tileExists[scx][scy + 1])
                        || !vSpoke || !hSpoke;
                    break;
                case 2: // BL: left exit, down exit, right(vSpoke), up(hSpoke)
                    canReach = (!hWalls[2 * scx][2 * scy] && scx > 0 && tileExists[scx - 1][scy])
                        || (!vWalls[2 * scx][2 * scy] && scy > 0 && tileExists[scx][scy - 1])
                        || !vSpoke || !hSpoke;
                    break;
                case 3: // BR: right exit, down exit, left(vSpoke), up(hSpoke)
                    canReach = (!hWalls[2 * scx + 2][2 * scy] && scx < x_size - 1 && tileExists[scx + 1][scy])
                        || (!vWalls[2 * scx][2 * scy] && scy > 0 && tileExists[scx][scy - 1])
                        || !vSpoke || !hSpoke;
                    break;
                }
                if (canReach)
                {
                    visited[scx][scy][sst] = true;
                    q.push({ scx, scy, sst });
                    count = 1;
                    foundStart = true;
                    break;
                }
            }
            if (foundStart) break;
        }

        if (!foundStart) return false;

        while (!q.empty())
        {
            auto [cx, cy, st] = q.front(); q.pop();

            bool hSpoke = cWalls[cx][cy][0] || cWalls[cx][cy][1];
            bool vSpoke = cWalls[cx][cy][2] || cWalls[cx][cy][3];

            auto tryVisit = [&](int ncx, int ncy, int nst)
                {
                    if (ncx < 0 || ncx >= x_size || ncy < 0 || ncy >= y_size) return;
                    if (!tileExists[ncx][ncy]) return;
                    if (visited[ncx][ncy][nst]) return;
                    visited[ncx][ncy][nst] = true;
                    count++;
                    q.push({ ncx, ncy, nst });
                };

            // ── Internal moves (same tile, blocked by spokes) ─────────────
            switch (st)
            {
            case 0: // TL
                if (!vSpoke) tryVisit(cx, cy, 1); // TL->TR
                if (!hSpoke) tryVisit(cx, cy, 2); // TL->BL
                break;
            case 1: // TR
                if (!vSpoke) tryVisit(cx, cy, 0); // TR->TL
                if (!hSpoke) tryVisit(cx, cy, 3); // TR->BR
                break;
            case 2: // BL
                if (!hSpoke) tryVisit(cx, cy, 0); // BL->TL
                if (!vSpoke) tryVisit(cx, cy, 3); // BL->BR
                break;
            case 3: // BR
                if (!hSpoke) tryVisit(cx, cy, 1); // BR->TR
                if (!vSpoke) tryVisit(cx, cy, 2); // BR->BL
                break;
            }

            // ── External moves (cross tile edge, blocked by edge walls) ───
            switch (st)
            {
            case 0: // TL -> TR of left neighbour / BL of top neighbour
                if (!hWalls[2 * cx][2 * cy])     tryVisit(cx - 1, cy, 1);
                if (!vWalls[2 * cx][2 * cy + 2])   tryVisit(cx, cy + 1, 2);
                break;
            case 1: // TR -> TL of right neighbour / BR of top neighbour
                if (!hWalls[2 * cx + 2][2 * cy])   tryVisit(cx + 1, cy, 0);
                if (!vWalls[2 * cx][2 * cy + 2])   tryVisit(cx, cy + 1, 3);
                break;
            case 2: // BL -> BR of left neighbour / TL of bottom neighbour
                if (!hWalls[2 * cx][2 * cy])     tryVisit(cx - 1, cy, 3);
                if (!vWalls[2 * cx][2 * cy])     tryVisit(cx, cy - 1, 0);
                break;
            case 3: // BR -> BL of right neighbour / TR of bottom neighbour
                if (!hWalls[2 * cx + 2][2 * cy])   tryVisit(cx + 1, cy, 2);
                if (!vWalls[2 * cx][2 * cy])     tryVisit(cx, cy - 1, 1);
                break;
            }
        }

        return count == total;
    }

    // Check that all 4 subtiles of cell (cx,cy) can each reach at least one
    // open exit to outside the cell, given the proposed spoke states.
    // Used as a fast pre-check before the full IsMapValid BFS.
    bool IsCellSubtileConnected(
        int cx, int cy,
        bool hSpoke,
        bool vSpoke,
        const bool hWalls[2 * MAP_SIZE + 1][2 * MAP_SIZE],
        const bool vWalls[2 * MAP_SIZE][2 * MAP_SIZE + 1],
        const std::vector<std::pair<int, int>>& activeTiles)
    {
        int cellId = cx * y_size + cy;

        bool leftBlocked = IsBorderWall(cellId, CellCardinalDirection::Left, activeTiles) || hWalls[2 * cx][2 * cy];
        bool rightBlocked = IsBorderWall(cellId, CellCardinalDirection::Right, activeTiles) || hWalls[2 * cx + 2][2 * cy];
        bool downBlocked = IsBorderWall(cellId, CellCardinalDirection::Down, activeTiles) || vWalls[2 * cx][2 * cy];
        bool upBlocked = IsBorderWall(cellId, CellCardinalDirection::Up, activeTiles) || vWalls[2 * cx][2 * cy + 2];

        bool hasExit[4] = {
            !upBlocked || !leftBlocked,   // TL
            !upBlocked || !rightBlocked,  // TR
            !downBlocked || !leftBlocked,   // BL
            !downBlocked || !rightBlocked,  // BR
        };

        bool adj[4][4] = {
            //         TL      TR       BL       BR
            /* TL */ { false, !vSpoke, !hSpoke,  false   },
            /* TR */ {!vSpoke, false,   false,   !hSpoke  },
            /* BL */ {!hSpoke, false,   false,   !vSpoke  },
            /* BR */ { false, !hSpoke, !vSpoke,  false    },
        };

        for (int start = 0; start < 4; start++)
        {
            bool visited[4] = {};
            std::queue<int> q;
            q.push(start);
            visited[start] = true;

            bool foundExit = hasExit[start];
            while (!q.empty() && !foundExit)
            {
                int cur = q.front(); q.pop();
                for (int nb = 0; nb < 4; nb++)
                {
                    if (adj[cur][nb] && !visited[nb])
                    {
                        visited[nb] = true;
                        if (hasExit[nb]) { foundExit = true; break; }
                        q.push(nb);
                    }
                }
            }

            if (!foundExit) return false;
        }

        return true;
    }

    // Returns true if enabling the wall on (cx,cy) in direction dir would seal
    // any subtile in either of the two cells sharing that wall edge.
    // Used as a fast pre-reject before the full IsMapValid BFS.
    bool WouldSealSubtile(
        int cx, int cy,
        CellCardinalDirection dir,
        const bool hWalls[2 * MAP_SIZE + 1][2 * MAP_SIZE],
        const bool vWalls[2 * MAP_SIZE][2 * MAP_SIZE + 1],
        const bool cWalls[MAP_SIZE][MAP_SIZE][4],
        const std::vector<std::pair<int, int>>& activeTiles)
    {
        int nx = cx, ny = cy;
        switch (dir)
        {
        case CellCardinalDirection::Left:  nx = cx - 1; break;
        case CellCardinalDirection::Right: nx = cx + 1; break;
        case CellCardinalDirection::Down:  ny = cy - 1; break;
        case CellCardinalDirection::Up:    ny = cy + 1; break;
        default: break;
        }

        auto checkCell = [&](int ccx, int ccy) -> bool
            {
                if (ccx < 0 || ccx >= x_size || ccy < 0 || ccy >= y_size) return true;
                bool hSpoke = cWalls[ccx][ccy][0] || cWalls[ccx][ccy][1];
                bool vSpoke = cWalls[ccx][ccy][2] || cWalls[ccx][ccy][3];
                return IsCellSubtileConnected(ccx, ccy, hSpoke, vSpoke, hWalls, vWalls, activeTiles);
            };

        return !checkCell(cx, cy) || !checkCell(nx, ny);
    }

    // Prints a full ASCII snapshot of the arena.
    //
    // Each active cell is a 4-char-wide block with shared borders.
    // Inactive (destroyed) cells are shown as blank space.
    // Grid is drawn top-to-bottom (cy = MAP_SIZE-1 at top).
    //
    // Legend:
    //   #   wall ON (border or toggled interior)
    //   .   open edge
    //   -   horizontal spoke ON
    //   |   vertical spoke ON
    //   +   corner/junction between two active cells (or active+border)
    //   ' ' inactive tile interior or no junction
    void PrintArenaState(EntityManager& entityManager)
    {
        // ── Gather active tiles ───────────────────────────────────────────────
        bool tileActive[MAP_SIZE][MAP_SIZE] = {};
        {
            auto tq = entityManager.CreateQuery<TileID>();
            for (auto [e, tid] : tq)
                if (tid->active)
                {
                    int cx = tid->id / y_size;
                    int cy = tid->id % y_size;
                    if (cx < MAP_SIZE && cy < MAP_SIZE)
                        tileActive[cx][cy] = true;
                }
        }

        std::unordered_set<Entity> spokeEntities;
        {
            auto sq = entityManager.CreateQuery<LaserWallID, CenterSpoke>();
            for (auto [e, lwid, spoke] : sq)
                spokeEntities.insert(e);
        }

        bool hWalls[2 * MAP_SIZE + 1][2 * MAP_SIZE] = {};
        bool vWalls[2 * MAP_SIZE][2 * MAP_SIZE + 1] = {};
        bool cWalls[MAP_SIZE][MAP_SIZE][4] = {};
        BuildWallMap(entityManager, spokeEntities, hWalls, vWalls, cWalls);

        // Bake border walls into hWalls/vWalls for the renderer
        {
            std::vector<std::pair<int, int>> activeTiles;
            for (int cx = 0; cx < MAP_SIZE; cx++)
                for (int cy = 0; cy < MAP_SIZE; cy++)
                    if (tileActive[cx][cy])
                        activeTiles.push_back({ cx, cy });

            for (auto& [cx, cy] : activeTiles)
            {
                int cellId = cx * y_size + cy;
                if (IsBorderWall(cellId, CellCardinalDirection::Left, activeTiles)) hWalls[2 * cx][2 * cy] = true;
                if (IsBorderWall(cellId, CellCardinalDirection::Right, activeTiles)) hWalls[2 * cx + 2][2 * cy] = true;
                if (IsBorderWall(cellId, CellCardinalDirection::Down, activeTiles)) vWalls[2 * cx][2 * cy] = true;
                if (IsBorderWall(cellId, CellCardinalDirection::Up, activeTiles)) vWalls[2 * cx][2 * cy + 2] = true;
            }
        }

        // ── Helpers ───────────────────────────────────────────────────────────

        // A '+' corner is printed at the grid point between cells.
        // Show '+' if at least one of the four adjacent cells is active.
        auto hasCorner = [&](int cx, int cy) -> bool
            {
                // cx,cy here are cell coordinates of the cell to the top-right of this corner
                // Adjacent cells: (cx-1,cy-1), (cx,cy-1), (cx-1,cy), (cx,cy)
                auto inBounds = [&](int x, int y) { return x >= 0 && x < MAP_SIZE && y >= 0 && y < MAP_SIZE; };
                return (inBounds(cx - 1, cy - 1) && tileActive[cx - 1][cy - 1]) ||
                    (inBounds(cx, cy - 1) && tileActive[cx][cy - 1]) ||
                    (inBounds(cx - 1, cy) && tileActive[cx - 1][cy]) ||
                    (inBounds(cx, cy) && tileActive[cx][cy]);
            };

        // Top wall char between corner(cx,cy) and corner(cx+1,cy):
        // show '#' or '.' only if the cell below (cx, cy-1) is active.
        auto topWallChar = [&](int cx, int cy) -> char
            {
                if (cy > 0 && tileActive[cx][cy - 1])
                    return vWalls[2 * cx][2 * (cy - 1) + 2] ? '#' : '.';
                if (cy < MAP_SIZE && tileActive[cx][cy])
                    return vWalls[2 * cx][2 * cy] ? '#' : '.';
                return ' ';
            };

        Debug::Info("Arena") << "=== Arena State (tile destroy in " << (int)tileDestroyTimer << "s) ===" << "\n";

        // Each cell prints: left_wall + 3 interior chars.
        // Then after the loop, the right wall of the last active cell is appended.
        // This way every cell owns its left wall and the rightmost active cell
        // closes itself — inactive cells in between contribute only spaces.

        for (int cy = MAP_SIZE - 1; cy >= 0; cy--)
        {
            // ── Top border row ────────────────────────────────────────────────
            {
                std::string row;
                for (int cx = 0; cx < MAP_SIZE; cx++)
                {
                    row += hasCorner(cx, cy + 1) ? '+' : ' ';
                    bool active = tileActive[cx][cy];
                    char tw = active ? (vWalls[2 * cx][2 * cy + 2] ? '#' : '.') : ' ';
                    row += tw; row += tw; row += tw;
                }
                row += hasCorner(MAP_SIZE, cy + 1) ? '+' : ' ';
                Debug::Info("Arena") << row << "\n";
            }

            // ── Upper subtile row (TL / TR) ───────────────────────────────────
            {
                std::string row;
                for (int cx = 0; cx < MAP_SIZE; cx++)
                {
                    bool active = tileActive[cx][cy];
                    bool vSpoke = active && (cWalls[cx][cy][2] || cWalls[cx][cy][3]);
                    // left wall of this cell
                    row += active ? (hWalls[2 * cx][2 * cy] ? '#' : '.') : ' ';
                    row += ' ';
                    row += (active && vSpoke) ? '|' : ' ';
                    row += ' ';
                    // right wall — only print if next cell is inactive or out of bounds
                    bool printRight = (cx == MAP_SIZE - 1) || !tileActive[cx + 1][cy];
                    if (printRight)
                        row += active ? (hWalls[2 * cx + 2][2 * cy] ? '#' : '.') : ' ';
                }
                Debug::Info("Arena") << row << "\n";
            }

            // ── Center spoke row ──────────────────────────────────────────────
            {
                std::string row;
                for (int cx = 0; cx < MAP_SIZE; cx++)
                {
                    bool active = tileActive[cx][cy];
                    bool hSpoke = active && (cWalls[cx][cy][0] || cWalls[cx][cy][1]);
                    bool vSpoke = active && (cWalls[cx][cy][2] || cWalls[cx][cy][3]);
                    char sh = (active && hSpoke) ? '-' : ' ';
                    char ctr = !active ? ' ' : (hSpoke && vSpoke) ? '+' : hSpoke ? '-' : vSpoke ? '|' : ' ';
                    row += active ? (hWalls[2 * cx][2 * cy] ? '#' : '.') : ' ';
                    row += sh;
                    row += ctr;
                    row += sh;
                    bool printRight = (cx == MAP_SIZE - 1) || !tileActive[cx + 1][cy];
                    if (printRight)
                        row += active ? (hWalls[2 * cx + 2][2 * cy] ? '#' : '.') : ' ';
                }
                Debug::Info("Arena") << row << "\n";
            }

            // ── Lower subtile row (BL / BR) ───────────────────────────────────
            {
                std::string row;
                for (int cx = 0; cx < MAP_SIZE; cx++)
                {
                    bool active = tileActive[cx][cy];
                    bool vSpoke = active && (cWalls[cx][cy][2] || cWalls[cx][cy][3]);
                    row += active ? (hWalls[2 * cx][2 * cy] ? '#' : '.') : ' ';
                    row += ' ';
                    row += (active && vSpoke) ? '|' : ' ';
                    row += ' ';
                    bool printRight = (cx == MAP_SIZE - 1) || !tileActive[cx + 1][cy];
                    if (printRight)
                        row += active ? (hWalls[2 * cx + 2][2 * cy] ? '#' : '.') : ' ';
                }
                Debug::Info("Arena") << row << "\n";
            }
        }

        // ── Bottom border row (cy = 0) ────────────────────────────────────────
        {
            std::string row;
            for (int cx = 0; cx < MAP_SIZE; cx++)
            {
                row += hasCorner(cx, 0) ? '+' : ' ';
                bool active = tileActive[cx][0];
                char bw = active ? (vWalls[2 * cx][0] ? '#' : '.') : ' ';
                row += bw; row += bw; row += bw;
            }
            row += hasCorner(MAP_SIZE, 0) ? '+' : ' ';
            Debug::Info("Arena") << row << "\n";
        }

        Debug::Info("Arena") << "========================================" << "\n";
    }

    void EmitToggleWall(std::vector<EventEntry>& events, int cellId,
        CellCardinalDirection dir, bool isSpoke, bool enabled,
        int cx, int cy)
    {
        EventEntry ev;
        ev.event.type = AsteroidEventMask::TOGGLE_WALL;
        ToggleWallEventData data;
        data.cellId = cellId;
        data.dir = dir;
        data.isSpoke = isSpoke;
        data.enabled = enabled;
        std::memcpy(ev.event.data, &data, sizeof(ToggleWallEventData));
        ev.event.len = sizeof(ToggleWallEventData);
        events.push_back(ev);
    }

    void EmitDestroyTile(std::vector<EventEntry>& events, int tileId)
    {
        Debug::Info("Arena") << "[SERVER] Destroying tile id=" << tileId
            << " cell=(" << tileId / y_size << "," << tileId % y_size << ")";
        EventEntry ev;
        ev.event.type = AsteroidEventMask::DESTROY_TILE;
        DestroyTileEventData data;
        data.tileId = tileId;
        std::memcpy(ev.event.data, &data, sizeof(DestroyTileEventData));
        ev.event.len = sizeof(DestroyTileEventData);
        events.push_back(ev);
    }

    void EmitWarnTile(std::vector<EventEntry>& events, int tileId)
    {
        EventEntry ev;
        ev.event.type = AsteroidEventMask::WARN_TILE;
        WarnTileEventData data;
        data.tileId = tileId;
        std::memcpy(ev.event.data, &data, sizeof(WarnTileEventData));
        ev.event.len = sizeof(WarnTileEventData);
        events.push_back(ev);
    }

    bool IsTileDestroyable(int cellId,
        const std::vector<std::pair<int, int>>& activeTiles)
    {
        if (activeTiles.size() <= 1) return false;

        int cx = cellId / y_size;
        int cy = cellId % y_size;

        std::vector<std::pair<int, int>> remaining;
        remaining.reserve(activeTiles.size() - 1);
        for (auto& t : activeTiles)
            if (!(t.first == cx && t.second == cy))
                remaining.push_back(t);

        if (remaining.empty()) return false;

        bool tileExists[MAP_SIZE][MAP_SIZE] = {};
        for (auto& t : remaining)
            tileExists[t.first][t.second] = true;

        bool visited[MAP_SIZE][MAP_SIZE] = {};
        std::queue<std::pair<int, int>> q;
        q.push(remaining[0]);
        visited[remaining[0].first][remaining[0].second] = true;
        int count = 1;

        while (!q.empty())
        {
            auto [tx, ty] = q.front(); q.pop();
            const int dx[] = { 1,-1,0,0 };
            const int dy[] = { 0,0,1,-1 };
            for (int d = 0; d < 4; d++)
            {
                int nx = tx + dx[d], ny = ty + dy[d];
                if (nx < 0 || nx >= x_size || ny < 0 || ny >= y_size) continue;
                if (!tileExists[nx][ny] || visited[nx][ny]) continue;
                visited[nx][ny] = true;
                count++;
                q.push({ nx,ny });
            }
        }

        return count == (int)remaining.size();
    }

public:
    void Update(EntityManager& entityManager, std::vector<EventEntry>& events,
        bool isServer, float deltaTime) override
    {
        if (!isServer) return;

        // ── Cache spoke entities once ─────────────────────────────────────────
        std::unordered_set<Entity> spokeEntities;
        {
            auto spokeQuery = entityManager.CreateQuery<LaserWallID, CenterSpoke>();
            for (auto [entity, lwid, spoke] : spokeQuery)
                spokeEntities.insert(entity);
        }

        // ── Cache active tiles once ───────────────────────────────────────────
        std::vector<std::pair<int, int>> activeTiles;
        activeTiles.reserve(MAP_SIZE * MAP_SIZE);
        {
            auto tileQuery = entityManager.CreateQuery<TileID>();
            for (auto [entity, tileId] : tileQuery)
                if (tileId->active)
                    activeTiles.push_back({ tileId->id / y_size, tileId->id % y_size });
        }

        // ── Step 1: update all timers and warning flags ───────────────────────
        {
            auto wallQuery = entityManager.CreateQuery<LaserWallID>();
            for (auto [entity, lwid] : wallQuery)
            {
                lwid->timer -= deltaTime;

                if (!spokeEntities.count(entity) &&
                    !IsBorderWall(lwid->cellId, lwid->dir, activeTiles))
                {
                    lwid->warning = (lwid->timer <= WARNING_THRESHOLD && lwid->timer > 0.0f);
                }
                else if (spokeEntities.count(entity))
                {
                    lwid->warning = (lwid->timer <= WARNING_THRESHOLD && lwid->timer > 0.0f);
                }
                else
                {
                    lwid->warning = false;
                }
            }
        }

        // ── Step 2: build wall map ────────────────────────────────────────────
        bool hWalls[2 * MAP_SIZE + 1][2 * MAP_SIZE];
        bool vWalls[2 * MAP_SIZE][2 * MAP_SIZE + 1];
        bool cWallsMap[MAP_SIZE][MAP_SIZE][4] = {};
        BuildWallMap(entityManager, spokeEntities, hWalls, vWalls, cWallsMap);

        // ── Step 3: enforce border walls and process expired interior walls ────
        {
            auto wallQuery = entityManager.CreateQuery<LaserWallID>();
            for (auto [entity, lwid] : wallQuery)
            {
                if (spokeEntities.count(entity)) continue;

                int cx = lwid->cellId / y_size;
                int cy = lwid->cellId % y_size;

                if (IsBorderWall(lwid->cellId, lwid->dir, activeTiles))
                {
                    if (!lwid->enabled)
                    {
                        lwid->enabled = true;
                        EmitToggleWall(events, lwid->cellId, lwid->dir, false, true, cx, cy);
                    }
                    lwid->warning = false;
                    continue;
                }

                if (lwid->timer > 0.0f) continue;

                bool newEnabled = !lwid->enabled;

                // Speculatively apply
                switch (lwid->dir)
                {
                case CellCardinalDirection::Down:  vWalls[2 * cx][2 * cy] = newEnabled; break;
                case CellCardinalDirection::Up:    vWalls[2 * cx][2 * cy + 2] = newEnabled; break;
                case CellCardinalDirection::Left:  hWalls[2 * cx][2 * cy] = newEnabled; break;
                case CellCardinalDirection::Right: hWalls[2 * cx + 2][2 * cy] = newEnabled; break;
                default: break;
                }

                // Accept if subtile-level connectivity holds across the whole map.
                // WouldSealSubtile is a fast pre-reject before the full BFS.
                bool valid = true;
                if (newEnabled)
                    valid = !WouldSealSubtile(cx, cy, lwid->dir, hWalls, vWalls, cWallsMap, activeTiles);
                if (valid)
                    valid = IsMapValid(activeTiles, hWalls, vWalls, cWallsMap);

                if (valid)
                {
                    lwid->enabled = newEnabled;
                    lwid->timer = RandomTimer();
                    lwid->warning = false;
                    EmitToggleWall(events, lwid->cellId, lwid->dir, false, newEnabled, cx, cy);
                }
                else
                {
                    // Revert speculative change
                    switch (lwid->dir)
                    {
                    case CellCardinalDirection::Down:  vWalls[2 * cx][2 * cy] = lwid->enabled; break;
                    case CellCardinalDirection::Up:    vWalls[2 * cx][2 * cy + 2] = lwid->enabled; break;
                    case CellCardinalDirection::Left:  hWalls[2 * cx][2 * cy] = lwid->enabled; break;
                    case CellCardinalDirection::Right: hWalls[2 * cx + 2][2 * cy] = lwid->enabled; break;
                    default: break;
                    }
                    lwid->timer = 1.0f;
                    lwid->warning = false;
                }
            }
        }

        // ── Step 4: center spokes ─────────────────────────────────────────────
        // Toggle ON: fast pre-check (IsCellSubtileConnected) then full BFS.
        // Toggle OFF: always valid — opening a passage can never disconnect.
        {
            auto spokeQuery = entityManager.CreateQuery<LaserWallID, CenterSpoke>();
            for (auto [entity, lwid, spoke] : spokeQuery)
            {
                if (lwid->timer > 0.0f) continue;

                int cx = lwid->cellId / y_size;
                int cy = lwid->cellId % y_size;
                bool newEnabled = !lwid->enabled;

                if (newEnabled)
                {
                    // Fast pre-check: would this spoke seal any subtile in its cell?
                    bool hSpoke = cWallsMap[cx][cy][0] || cWallsMap[cx][cy][1];
                    bool vSpoke = cWallsMap[cx][cy][2] || cWallsMap[cx][cy][3];

                    switch (lwid->dir)
                    {
                    case CellCardinalDirection::Down:
                    case CellCardinalDirection::Up:    hSpoke = true; break;
                    case CellCardinalDirection::Left:
                    case CellCardinalDirection::Right: vSpoke = true; break;
                    default: break;
                    }

                    if (!IsCellSubtileConnected(cx, cy, hSpoke, vSpoke, hWalls, vWalls, activeTiles))
                    {
                        lwid->timer = 1.0f;
                        lwid->warning = false;
                        continue;
                    }

                    // Full subtile-level connectivity check
                    switch (lwid->dir)
                    {
                    case CellCardinalDirection::Down:  cWallsMap[cx][cy][0] = true; break;
                    case CellCardinalDirection::Up:    cWallsMap[cx][cy][1] = true; break;
                    case CellCardinalDirection::Left:  cWallsMap[cx][cy][2] = true; break;
                    case CellCardinalDirection::Right: cWallsMap[cx][cy][3] = true; break;
                    default: break;
                    }

                    if (!IsMapValid(activeTiles, hWalls, vWalls, cWallsMap))
                    {
                        // Revert
                        switch (lwid->dir)
                        {
                        case CellCardinalDirection::Down:  cWallsMap[cx][cy][0] = false; break;
                        case CellCardinalDirection::Up:    cWallsMap[cx][cy][1] = false; break;
                        case CellCardinalDirection::Left:  cWallsMap[cx][cy][2] = false; break;
                        case CellCardinalDirection::Right: cWallsMap[cx][cy][3] = false; break;
                        default: break;
                        }
                        lwid->timer = 1.0f;
                        lwid->warning = false;
                        continue;
                    }
                }
                else
                {
                    // Toggling OFF — update cWallsMap so subsequent spokes this frame see it
                    switch (lwid->dir)
                    {
                    case CellCardinalDirection::Down:  cWallsMap[cx][cy][0] = false; break;
                    case CellCardinalDirection::Up:    cWallsMap[cx][cy][1] = false; break;
                    case CellCardinalDirection::Left:  cWallsMap[cx][cy][2] = false; break;
                    case CellCardinalDirection::Right: cWallsMap[cx][cy][3] = false; break;
                    default: break;
                    }
                }

                lwid->enabled = newEnabled;
                lwid->timer = RandomTimer();
                lwid->warning = false;
                EmitToggleWall(events, lwid->cellId, lwid->dir, true, newEnabled, cx, cy);
            }
        }

        // ── Step 5: tile destruction ──────────────────────────────────────────
        tileDestroyTimer -= deltaTime;

        if (!tileWarningEmitted && tileDestroyTimer <= TILE_WARNING_THRESHOLD && pendingDestroyTileId == -1)
        {
            std::vector<int> candidates;
            for (auto& t : activeTiles)
            {
                int cellId = t.first * y_size + t.second;
                if (IsTileDestroyable(cellId, activeTiles))
                    candidates.push_back(cellId);
            }

            if (!candidates.empty())
            {
                std::uniform_int_distribution<int> pick(0, (int)candidates.size() - 1);
                pendingDestroyTileId = candidates[pick(rng)];
                tileWarningEmitted = true;
                EmitWarnTile(events, pendingDestroyTileId);
            }
            else
            {
                tileDestroyTimer = TILE_DESTROY_INTERVAL;
                tileWarningEmitted = false;
                pendingDestroyTileId = -1;
            }
        }

        if (tileDestroyTimer <= 0.0f)
        {
            if (pendingDestroyTileId != -1)
                EmitDestroyTile(events, pendingDestroyTileId);
            tileDestroyTimer = TILE_DESTROY_INTERVAL;
            tileWarningEmitted = false;
            pendingDestroyTileId = -1;
        }

        // ── Debug print ───────────────────────────────────────────────────────
        debugPrintArenaTimer -= deltaTime;
        if (debugPrintArenaTimer <= 0.0f)
        {
            PrintArenaState(entityManager);
            debugPrintArenaTimer = 10.0f;
        }
    }
};

class InputServerSystem : public ISystem {
public:
    void Update(EntityManager& entityManager, std::vector<EventEntry>& events, bool isServer, float deltaTime) override {
        auto query = entityManager.CreateQuery<Transform, Playable, SpaceShip>();
        for (auto [entity, transform, play, ship] : query) {
            int p = play->playerId;
            InputBlob input = play->input;
            uint8_t m = input.data[0];

            const float BULLET_SPEED = 5.0f;
            const int SHOOT_COOLDOWN = 10;

            float radians = transform->getRotation().z * 3.14159f / 180.0f;

            if (ship->shootCooldown > 0) ship->shootCooldown--;
            if (ship->remainingShootFrames > 0) ship->remainingShootFrames--;

            if (ship->remainingShootFrames == 0 && ship->isShooting) {
                ship->isShooting = false;

                if (ship->isAlive) {
                    bool usedIds[MAX_BULLETS] = { false };
                    auto query2 = entityManager.CreateQuery<ECSBullet>();
                    for (auto [entity, ecsb] : query2)
                        if (ecsb->id >= 0 && ecsb->id < MAX_BULLETS)
                            usedIds[ecsb->id] = true;

                    int id = -1;
                    for (int i = 0; i < MAX_BULLETS; i++)
                        if (!usedIds[i]) { id = i; break; }

                    if (id != -1) {
                        float bVelX = cos(radians) * BULLET_SPEED;
                        float bVelY = sin(radians) * BULLET_SPEED;

                        EventEntry spawnEvent;
                        spawnEvent.event.type = AsteroidEventMask::SPAWN_BULLET;
                        SpawnBulletEventData spawnData;
                        spawnData.bulletId = id;
                        spawnData.ownerId = p;
                        spawnData.posX = transform->getPosition().x;
                        spawnData.posY = transform->getPosition().y;
                        spawnData.velX = bVelX;
                        spawnData.velY = bVelY;
                        std::memcpy(spawnEvent.event.data, &spawnData, sizeof(SpawnBulletEventData));
                        spawnEvent.event.len = sizeof(SpawnBulletEventData);
                        events.push_back(spawnEvent);

                        ship->shootCooldown = SHOOT_COOLDOWN;
                    }
                }
            }
        }
    }
};

class OnDeathLogicSystem : public ISystem {
public:
    void Update(EntityManager& entityManager, std::vector<EventEntry>& events, bool isServer, float deltaTime) override {
        auto playerQuery = entityManager.CreateQuery<Transform, Playable, SpaceShip>();
        for (auto [entity, playerTransform, play, ship] : playerQuery) {
            if (!(ship->isAlive)) {
                ship->deathCooldown--;
                if (ship->deathCooldown <= 0) {
                    EventEntry respawnEvent;
                    respawnEvent.event.type = AsteroidEventMask::RESPAWN;
                    RespawnEventData respawnData;
                    respawnData.playerId = play->playerId;
                    std::memcpy(respawnEvent.event.data, &respawnData, sizeof(RespawnEventData));
                    respawnEvent.event.len = sizeof(RespawnEventData);
                    events.push_back(respawnEvent);
                }
            }
        }
    }
};