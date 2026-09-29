#pragma once

#include <cstring>
#include <queue>
#include <random>
#include <string>
#include <tuple>
#include <unordered_set>
#include <utility>
#include <vector>

#include "ecs/ecs_common.hpp"
#include "ecs/Collisions/BoxCollider2D.hpp"
#include "Utils/Debug/Debug.hpp"
#include "../Components.hpp"
#include "../Events.hpp"
#include "../GameState.hpp"

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
    bool  initialValidationDone = false;

    // Frame being simulated (the input state's frame); the output state is currentFrame + 1. Set by
    // AsteroidShooterGame::SimulateFrame before each tick.
    int   currentFrame = 0;

    float RandomTimer()
    {
        std::uniform_real_distribution<float> dist(3.0f, 30.0f);
        return dist(rng);
    }

    // How many more "t -= dt" steps until t <= 0, doing the exact same float operations the timers do, so the
    // predicted on-frame matches the tick the toggle really happens in.
    static int TicksUntilNonPositive(float t, float dt)
    {
        int n = 0;
        while (t > 0.0f) { t -= dt; ++n; }
        return n;
    }

    // Subtile graph. Each cell has four subtiles (0=UL, 1=UR, 2=DL, 3=DR); each touches one HALF of two cell sides
    // (EdgeWallSlot), which lead to the facing subtile of the neighbouring cell, and each centre spoke separates exactly
    // one pair of subtiles of its cell. All maze checks below walk this graph, at half-wall resolution.
    struct SubtileExit { CellCardinalDirection dir; int half; int neighborSubtile; };
    static constexpr SubtileExit kSubtileExits[4][2] = {
        { { CellCardinalDirection::Left,  1, 1 }, { CellCardinalDirection::Up,   0, 2 } },   // UL
        { { CellCardinalDirection::Right, 1, 0 }, { CellCardinalDirection::Up,   1, 3 } },   // UR
        { { CellCardinalDirection::Left,  0, 3 }, { CellCardinalDirection::Down, 0, 0 } },   // DL
        { { CellCardinalDirection::Right, 0, 2 }, { CellCardinalDirection::Down, 1, 1 } },   // DR
    };

    // Up spoke (upper half of the vertical centreline) splits UL|UR, Down splits DL|DR; Left (left half of the
    // horizontal centreline) splits UL/DL, Right splits UR/DR. `spoke` indexes cWalls (SpokeIndex).
    struct SubtileLink { int a, b, spoke; };
    static constexpr SubtileLink kSubtileLinks[4] = { { 0, 1, 1 }, { 2, 3, 0 }, { 0, 2, 2 }, { 1, 3, 3 } };

    static constexpr CellCardinalDirection kSideDirs[4] = {
        CellCardinalDirection::Left, CellCardinalDirection::Right,
        CellCardinalDirection::Down, CellCardinalDirection::Up
    };

    // Would turning this (off) border/shared half-wall on be accepted right now? Same checks as Step 3's toggle, on the
    // maps as they stand; the maps are restored before returning.
    bool CanEnableEdgeWall(const LaserWallID& w,
        bool hWalls[2 * MAP_SIZE + 1][2 * MAP_SIZE],
        bool vWalls[2 * MAP_SIZE][2 * MAP_SIZE + 1],
        bool cWalls[MAP_SIZE][MAP_SIZE][4],
        const std::vector<std::pair<int, int>>& activeTiles)
    {
        const int cx = w.cellId / y_size;
        const int cy = w.cellId % y_size;

        bool& slot = EdgeWallSlot(hWalls, vWalls, w.cellId, w.dir, w.half);
        const bool old = slot;
        slot = true;
        const bool valid = !WouldSealSubtile(cx, cy, w.dir, hWalls, vWalls, cWalls, activeTiles)
            && IsMapValid(activeTiles, hWalls, vWalls, cWalls);
        slot = old;
        return valid;
    }

    // Same for a centre spoke, mirroring Step 4's checks.
    bool CanEnableSpoke(const LaserWallID& w,
        bool hWalls[2 * MAP_SIZE + 1][2 * MAP_SIZE],
        bool vWalls[2 * MAP_SIZE][2 * MAP_SIZE + 1],
        bool cWalls[MAP_SIZE][MAP_SIZE][4],
        const std::vector<std::pair<int, int>>& activeTiles)
    {
        const int cx = w.cellId / y_size;
        const int cy = w.cellId % y_size;

        bool& slot = cWalls[cx][cy][SpokeIndex(w.dir)];
        const bool old = slot;
        slot = true;
        const bool valid = IsCellSubtileConnected(cx, cy, hWalls, vWalls, cWalls, activeTiles)
            && IsMapValid(activeTiles, hWalls, vWalls, cWalls);
        slot = old;
        return valid;
    }

    void SyncCollider(EntityManager& entityManager, Entity entity, bool enabled)
    {
        auto* col = entityManager.GetComponent<BoxCollider2D>(entity);
        if (col) col->isEnabled = enabled;
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

    // Marks both halves of every side facing the void (map edge or destroyed cell) as walled, as Step 3 enforces them.
    void MarkBorderWalls(const std::vector<std::pair<int, int>>& activeTiles,
        bool hWalls[2 * MAP_SIZE + 1][2 * MAP_SIZE],
        bool vWalls[2 * MAP_SIZE][2 * MAP_SIZE + 1])
    {
        for (auto& [cx, cy] : activeTiles)
        {
            const int cellId = cx * y_size + cy;
            for (CellCardinalDirection dir : kSideDirs)
                if (IsBorderWall(cellId, dir, activeTiles))
                    for (int half = 0; half < 2; half++)
                        EdgeWallSlot(hWalls, vWalls, cellId, dir, half) = true;
        }
    }

    // NeighborCellId/OppositeDirection/ClassifyWallEdge live in Components.hpp: LaserWallRenderSystem needs the
    // same edge classification, and duplicating it here would risk the two drifting apart.

    // A shared half-edge is one LaserWallID stored under one of its two cells. FixInitialReachability asks for "cell
    // X's wall in direction D, half H", but it may be stored from the NEIGHBOUR's (opposite) side, with the same half
    // (see EdgeWallSlot) — this checks both.
    Entity FindWallEntity(EntityManager& entityManager,
        const std::unordered_set<Entity>& spokeEntities,
        int cellId, CellCardinalDirection dir, int half)
    {
        int neighborCellId = NeighborCellId(cellId, dir);
        CellCardinalDirection oppositeDir = OppositeDirection(dir);

        auto wallQuery = entityManager.CreateQuery<LaserWallID>();
        for (auto [entity, lwid] : wallQuery)
        {
            if (spokeEntities.count(entity)) continue;
            if (lwid->half != half) continue;
            if (lwid->cellId == cellId && lwid->dir == dir) return entity;
            if (neighborCellId != -1 && lwid->cellId == neighborCellId && lwid->dir == oppositeDir)
                return entity;
        }
        return 0;
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

            int cx = lwid->cellId / y_size;
            int cy = lwid->cellId % y_size;

            if (spokeEntities.count(entity))
            {
                // Spokes are single-owner (never a shared edge): only their
                // own cell matters.
                if (!activeTileSet.count(lwid->cellId)) continue;
                cWalls[cx][cy][SpokeIndex(lwid->dir)] = true;
            }
            else
            {
                // Shared edge: a stale "enabled" from before its cell died would read as live (BuildWallMap runs before Step 3).
                // Skip only if NEITHER bordering cell is active — checking just lwid->cellId would drop SoleBorder walls stored
                // under the dead side.
                if (ClassifyWallEdge(lwid->cellId, lwid->dir, activeTileSet) == WallEdgeState::Dead)
                    continue;

                EdgeWallSlot(hWalls, vWalls, lwid->cellId, lwid->dir, lwid->half) = true;
            }
        }
    }

    // Every subtile of every active tile reachable from any other.
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

        bool visited[MAP_SIZE][MAP_SIZE][4] = {};
        const int total = (int)activeTiles.size() * 4;
        int count = 0;

        std::queue<std::tuple<int, int, int>> q;
        auto tryVisit = [&](int ncx, int ncy, int nst)
            {
                if (ncx < 0 || ncx >= x_size || ncy < 0 || ncy >= y_size) return;
                if (!tileExists[ncx][ncy]) return;
                if (visited[ncx][ncy][nst]) return;
                visited[ncx][ncy][nst] = true;
                count++;
                q.push({ ncx, ncy, nst });
            };

        tryVisit(activeTiles[0].first, activeTiles[0].second, 0);

        while (!q.empty())
        {
            auto [cx, cy, st] = q.front(); q.pop();

            for (const SubtileLink& link : kSubtileLinks)
            {
                if (cWalls[cx][cy][link.spoke]) continue;
                if (link.a == st) tryVisit(cx, cy, link.b);
                else if (link.b == st) tryVisit(cx, cy, link.a);
            }

            const int cellId = cx * y_size + cy;
            for (const SubtileExit& exit : kSubtileExits[st])
            {
                if (EdgeWallSlot(hWalls, vWalls, cellId, exit.dir, exit.half)) continue;
                const int n = NeighborCellId(cellId, exit.dir);
                if (n == -1) continue;
                tryVisit(n / y_size, n % y_size, exit.neighborSubtile);
            }
        }

        return count == total;
    }

    // Every subtile of cell (cx,cy) can walk (inside the cell, around its spokes) to an open half-side leading to an
    // active neighbour.
    bool IsCellSubtileConnected(
        int cx, int cy,
        const bool hWalls[2 * MAP_SIZE + 1][2 * MAP_SIZE],
        const bool vWalls[2 * MAP_SIZE][2 * MAP_SIZE + 1],
        const bool cWalls[MAP_SIZE][MAP_SIZE][4],
        const std::vector<std::pair<int, int>>& activeTiles)
    {
        const int cellId = cx * y_size + cy;

        bool hasExit[4] = {};
        for (int st = 0; st < 4; st++)
            for (const SubtileExit& exit : kSubtileExits[st])
                if (!IsBorderWall(cellId, exit.dir, activeTiles) &&
                    !EdgeWallSlot(hWalls, vWalls, cellId, exit.dir, exit.half))
                    hasExit[st] = true;

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
                for (const SubtileLink& link : kSubtileLinks)
                {
                    if (cWalls[cx][cy][link.spoke]) continue;
                    int nb = (link.a == cur) ? link.b : (link.b == cur) ? link.a : -1;
                    if (nb == -1 || visited[nb]) continue;
                    visited[nb] = true;
                    if (hasExit[nb]) { foundExit = true; break; }
                    q.push(nb);
                }
            }
            if (!foundExit) return false;
        }
        return true;
    }

    bool WouldSealSubtile(
        int cx, int cy, CellCardinalDirection dir,
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
                return IsCellSubtileConnected(ccx, ccy, hWalls, vWalls, cWalls, activeTiles);
            };
        return !checkCell(cx, cy) || !checkCell(nx, ny);
    }

    void FixInitialReachability(
        EntityManager& entityManager,
        const std::unordered_set<Entity>& spokeEntities,
        const std::vector<std::pair<int, int>>& activeTiles,
        bool hWalls[2 * MAP_SIZE + 1][2 * MAP_SIZE],
        bool vWalls[2 * MAP_SIZE][2 * MAP_SIZE + 1],
        bool cWallsMap[MAP_SIZE][MAP_SIZE][4],
        std::vector<EventEntry>& events)
    {
        // Each pass opens at most one half-wall or spoke; a cell has 8 half-walls and 4 spokes.
        for (int pass = 0; pass < MAP_SIZE * MAP_SIZE * 12; pass++)
        {
            if (IsMapValid(activeTiles, hWalls, vWalls, cWallsMap)) break;

            bool fixedAny = false;
            for (auto& [cx, cy] : activeTiles)
            {
                if (IsCellSubtileConnected(cx, cy, hWalls, vWalls, cWallsMap, activeTiles))
                    continue;

                const int cellId = cx * y_size + cy;

                // Open one (non-border) half-wall that reconnects the cell...
                for (CellCardinalDirection dir : kSideDirs)
                {
                    if (IsBorderWall(cellId, dir, activeTiles)) continue;

                    for (int half = 0; half < 2 && !fixedAny; half++)
                    {
                        bool& slot = EdgeWallSlot(hWalls, vWalls, cellId, dir, half);
                        if (!slot) continue;

                        slot = false;
                        if (IsCellSubtileConnected(cx, cy, hWalls, vWalls, cWallsMap, activeTiles))
                        {
                            // The entity for (cellId,dir,half) may actually be stored
                            // under the neighbour's opposite-direction view.
                            Entity wallEntity = FindWallEntity(entityManager, spokeEntities, cellId, dir, half);
                            LaserWallID* lwid = wallEntity ? entityManager.GetComponent<LaserWallID>(wallEntity) : nullptr;
                            if (lwid && lwid->enabled)
                            {
                                lwid->enabled = false;
                                lwid->timer = RandomTimer();
                                SyncCollider(entityManager, wallEntity, false);
                            }
                            fixedAny = true;
                        }
                        else
                        {
                            slot = true;
                        }
                    }
                    if (fixedAny) break;
                }

                if (fixedAny) break;

                // ...or else one spoke.
                for (int dirIdx = 0; dirIdx < 4 && !fixedAny; dirIdx++)
                {
                    if (!cWallsMap[cx][cy][dirIdx]) continue;

                    cWallsMap[cx][cy][dirIdx] = false;
                    if (!IsCellSubtileConnected(cx, cy, hWalls, vWalls, cWallsMap, activeTiles))
                    {
                        cWallsMap[cx][cy][dirIdx] = true;
                        continue;
                    }

                    auto spokeQuery = entityManager.CreateQuery<LaserWallID, CenterSpoke>();
                    for (auto [entity, lwid, spoke] : spokeQuery)
                    {
                        if (lwid->cellId != cellId || SpokeIndex(lwid->dir) != dirIdx) continue;
                        if (!lwid->enabled) continue;
                        lwid->enabled = false;
                        lwid->timer = RandomTimer();
                        SyncCollider(entityManager, entity, false);
                        break;
                    }
                    fixedAny = true;
                }

                if (fixedAny) break;
            }

            if (!fixedAny) break;
        }
    }

    // Per tile row: top side (two halves around the middle pillar), upper subtiles with the Up spoke, the centre line
    // (Left/Right spokes), lower subtiles with the Down spoke. '#' wall on, '.' wall off.
    void PrintArenaState(EntityManager& entityManager)
    {
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

        {
            std::vector<std::pair<int, int>> activeTiles;
            for (int cx = 0; cx < MAP_SIZE; cx++)
                for (int cy = 0; cy < MAP_SIZE; cy++)
                    if (tileActive[cx][cy])
                        activeTiles.push_back({ cx, cy });
            MarkBorderWalls(activeTiles, hWalls, vWalls);
        }

        auto hasCorner = [&](int cx, int cy) -> bool
            {
                auto inBounds = [&](int x, int y) { return x >= 0 && x < MAP_SIZE && y >= 0 && y < MAP_SIZE; };
                return (inBounds(cx - 1, cy - 1) && tileActive[cx - 1][cy - 1]) ||
                    (inBounds(cx, cy - 1) && tileActive[cx][cy - 1]) ||
                    (inBounds(cx - 1, cy) && tileActive[cx - 1][cy]) ||
                    (inBounds(cx, cy) && tileActive[cx][cy]);
            };
        auto wallChar = [](bool active, bool on) { return !active ? ' ' : (on ? '#' : '.'); };

        Debug::Info("Arena") << "=== Arena State (tile destroy in " << (int)tileDestroyTimer << "s) ===" << "\n";

        for (int cy = MAP_SIZE - 1; cy >= 0; cy--)
        {
            {
                std::string row;
                for (int cx = 0; cx < MAP_SIZE; cx++)
                {
                    const bool active = tileActive[cx][cy];
                    row += hasCorner(cx, cy + 1) ? '+' : ' ';
                    row += wallChar(active, vWalls[2 * cx][2 * cy + 2]);
                    row += active ? '+' : ' ';
                    row += wallChar(active, vWalls[2 * cx + 1][2 * cy + 2]);
                }
                row += hasCorner(MAP_SIZE, cy + 1) ? '+' : ' ';
                Debug::Info("Arena") << row << "\n";
            }
            {
                std::string row;
                for (int cx = 0; cx < MAP_SIZE; cx++)
                {
                    const bool active = tileActive[cx][cy];
                    row += wallChar(active, hWalls[2 * cx][2 * cy + 1]);
                    row += ' ';
                    row += (active && cWalls[cx][cy][1]) ? '|' : ' ';
                    row += ' ';
                    bool printRight = (cx == MAP_SIZE - 1) || !tileActive[cx + 1][cy];
                    if (printRight) row += wallChar(active, hWalls[2 * cx + 2][2 * cy + 1]);
                }
                Debug::Info("Arena") << row << "\n";
            }
            {
                std::string row;
                for (int cx = 0; cx < MAP_SIZE; cx++)
                {
                    const bool active = tileActive[cx][cy];
                    row += active ? '+' : ' ';
                    row += (active && cWalls[cx][cy][2]) ? '-' : ' ';
                    row += active ? '+' : ' ';
                    row += (active && cWalls[cx][cy][3]) ? '-' : ' ';
                    bool printRight = (cx == MAP_SIZE - 1) || !tileActive[cx + 1][cy];
                    if (printRight) row += active ? '+' : ' ';
                }
                Debug::Info("Arena") << row << "\n";
            }
            {
                std::string row;
                for (int cx = 0; cx < MAP_SIZE; cx++)
                {
                    const bool active = tileActive[cx][cy];
                    row += wallChar(active, hWalls[2 * cx][2 * cy]);
                    row += ' ';
                    row += (active && cWalls[cx][cy][0]) ? '|' : ' ';
                    row += ' ';
                    bool printRight = (cx == MAP_SIZE - 1) || !tileActive[cx + 1][cy];
                    if (printRight) row += wallChar(active, hWalls[2 * cx + 2][2 * cy]);
                }
                Debug::Info("Arena") << row << "\n";
            }
        }
        {
            std::string row;
            for (int cx = 0; cx < MAP_SIZE; cx++)
            {
                const bool active = tileActive[cx][0];
                row += hasCorner(cx, 0) ? '+' : ' ';
                row += wallChar(active, vWalls[2 * cx][0]);
                row += active ? '+' : ' ';
                row += wallChar(active, vWalls[2 * cx + 1][0]);
            }
            row += hasCorner(MAP_SIZE, 0) ? '+' : ' ';
            Debug::Info("Arena") << row << "\n";
        }
        Debug::Info("Arena") << "========================================" << "\n";
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
                q.push({ nx, ny });
            }
        }

        return count == (int)remaining.size();
    }

public:
    void SetCurrentFrame(int frame) { currentFrame = frame; }

    void Update(EntityManager& entityManager, std::vector<EventEntry>& events,
        bool isServer, float deltaTime) override
    {
        if (!isServer) return;

        // Pre-match freeze: no wall toggling, no tile destruction timer
        // running, until the countdown reaches 0.
        {
            auto timerQuery = entityManager.CreateQuery<MatchStartTimer>();
            for (auto [entity, timer] : timerQuery)
                if (timer->ticksRemaining > 0) return;
        }

        // Cache spoke entities once
        std::unordered_set<Entity> spokeEntities;
        {
            auto spokeQuery = entityManager.CreateQuery<LaserWallID, CenterSpoke>();
            for (auto [entity, lwid, spoke] : spokeQuery)
                spokeEntities.insert(entity);
        }

        // Cache active tiles once
        std::vector<std::pair<int, int>> activeTiles;
        activeTiles.reserve(MAP_SIZE * MAP_SIZE);
        std::unordered_set<int> activeCellIds;
        {
            auto tileQuery = entityManager.CreateQuery<TileID>();
            for (auto [entity, tileId] : tileQuery)
                if (tileId->active)
                {
                    activeTiles.push_back({ tileId->id / y_size, tileId->id % y_size });
                    activeCellIds.insert(tileId->id);
                }
        }

        // Off walls whose random-toggle warning starts this tick; validated after Step 2 (needs the wall maps).
        std::vector<Entity> newlyWarnedOff;

        // Step 1: update all timers and the warning flag (synced to clients via DELTA_WALL_STATE)
        {
            auto wallQuery = entityManager.CreateQuery<LaserWallID>();
            for (auto [entity, lwid] : wallQuery)
            {
                lwid->timer -= deltaTime;
                const bool wasWarning = lwid->warning;
                lwid->onFrame = -1;

                bool isSpoke = spokeEntities.count(entity) > 0;
                bool notInterior = !isSpoke &&
                    ClassifyWallEdge(lwid->cellId, lwid->dir, activeCellIds) != WallEdgeState::Interior;

                // A spoke of a destroyed tile is forced off in Step 4 whatever its timer: nothing to warn about.
                if (notInterior || (isSpoke && !activeCellIds.count(lwid->cellId)))
                {
                    lwid->warning = false;
                    continue;
                }

                const bool randomWindow = (lwid->timer <= WARNING_THRESHOLD && lwid->timer > 0.0f);
                bool inWarningWindow = randomWindow;

                // A wall bordering the tile scheduled for destruction becomes a solid border wall when it disappears. Warn
                // beforehand, like the tile's own TILE_WARNING_THRESHOLD blink, instead of snapping on with no warning.
                // Not spokes: those of the doomed tile are forced off with it, never on.
                const bool tilePending = !isSpoke && !lwid->enabled && pendingDestroyTileId != -1 &&
                    (lwid->cellId == pendingDestroyTileId ||
                     NeighborCellId(lwid->cellId, lwid->dir) == pendingDestroyTileId);
                if (tilePending)
                {
                    inWarningWindow = true;
                }

                if (inWarningWindow && !lwid->warning)
                {
                    lwid->warning = true;
                }
                else if (!inWarningWindow && lwid->warning)
                {
                    lwid->warning = false;
                }
                else if (!inWarningWindow)
                {
                    lwid->warning = false;
                }

                // Off wall about to energise: the state frame it will show up on in (see EncodeWallWarning). The random
                // toggle happens in the tick whose decrement takes the timer to <= 0 (Step 3/4 of that same tick), so it's
                // in the next state. The tile's destruction likewise: Step 5 emits it in the tick tileDestroyTimer runs
                // out (decremented after this, in this same tick), DestroyTileHandler applies it at the start of the next
                // and Step 3 enforces the new border wall in it, so the wall shows in the state after that. Both come out
                // as currentFrame + n + 1. CollisionSystem runs before this system, so the wall first kills a ship
                // overlapping it in that same state frame: exactly when the client should draw it on.
                // (A spoke of the tile about to be destroyed may turn on, but is then forced off with the tile: its
                // on-frame isn't a promise, so it keeps the plain warning.)
                const bool spokeOfDoomedTile = isSpoke && lwid->cellId == pendingDestroyTileId;
                if (lwid->warning && !lwid->enabled && !spokeOfDoomedTile)
                {
                    int onFrame = -1;
                    if (randomWindow)
                        onFrame = currentFrame + TicksUntilNonPositive(lwid->timer, deltaTime) + 1;
                    if (tilePending)
                    {
                        const int tileOnFrame = currentFrame + TicksUntilNonPositive(tileDestroyTimer, deltaTime) + 1;
                        onFrame = (onFrame < 0) ? tileOnFrame : std::min(onFrame, tileOnFrame);
                    }
                    lwid->onFrame = onFrame;

                    if (!wasWarning && randomWindow && !tilePending)
                        newlyWarnedOff.push_back(entity);
                }
            }
        }

        // Step 2: build wall map
        bool hWalls[2 * MAP_SIZE + 1][2 * MAP_SIZE];
        bool vWalls[2 * MAP_SIZE][2 * MAP_SIZE + 1];
        bool cWallsMap[MAP_SIZE][MAP_SIZE][4] = {};
        BuildWallMap(entityManager, spokeEntities, hWalls, vWalls, cWallsMap);

        // Step 0: initial reachability fix
        if (!initialValidationDone)
        {
            MarkBorderWalls(activeTiles, hWalls, vWalls);

            FixInitialReachability(entityManager, spokeEntities, activeTiles,
                hWalls, vWalls, cWallsMap, events);
            BuildWallMap(entityManager, spokeEntities, hWalls, vWalls, cWallsMap);
            initialValidationDone = true;
        }

        // Step 2b: commit to a random turn-on when its warning starts. Clients draw the wall on at its predicted frame
        // (see EncodeWallWarning), so a toggle rejected only when it expires would show a wall that never came on (for
        // the client's prediction lead). Checking now with the same rules re-rolls it instead, with no warning at all
        // (which also drops the old warnings that led to nothing). Step 3/4 still re-check at expiry, since the maze can
        // change in between; that rare case is the only one left where a client briefly sees a wall that didn't happen.
        for (Entity entity : newlyWarnedOff)
        {
            LaserWallID* lwid = entityManager.GetComponent<LaserWallID>(entity);
            if (!lwid) continue;

            const bool canEnable = spokeEntities.count(entity)
                ? CanEnableSpoke(*lwid, hWalls, vWalls, cWallsMap, activeTiles)
                : CanEnableEdgeWall(*lwid, hWalls, vWalls, cWallsMap, activeTiles);
            if (!canEnable)
            {
                lwid->warning = false;
                lwid->onFrame = -1;
                lwid->timer = RandomTimer();
            }
        }

        // Step 3: enforce border walls and process expired interior walls
        {
            auto wallQuery = entityManager.CreateQuery<LaserWallID>();
            for (auto [entity, lwid] : wallQuery)
            {
                if (spokeEntities.count(entity)) continue;

                int cx = lwid->cellId / y_size;
                int cy = lwid->cellId % y_size;

                // ClassifyWallEdge checks BOTH bordering cells: with one entity per shared edge, the surviving side
                // isn't necessarily the one it's stored under.
                WallEdgeState edgeState = ClassifyWallEdge(lwid->cellId, lwid->dir, activeCellIds);

                if (edgeState == WallEdgeState::Dead)
                {
                    // Neither bordering cell exists anymore (or this is a
                    // map-edge wall whose only cell died): nothing to
                    // protect, force off and stop touching it.
                    if (lwid->enabled)
                    {
                        lwid->enabled = false;
                        SyncCollider(entityManager, entity, false);
                    }
                    if (lwid->warning) lwid->warning = false;
                    continue;
                }

                if (edgeState == WallEdgeState::SoleBorder)
                {
                    // Exactly one side is alive: solid, un-toggleable wall
                    // protecting that survivor from the void on the other
                    // side (whether that's the map edge or a destroyed cell).
                    if (!lwid->enabled)
                    {
                        lwid->enabled = true;
                        SyncCollider(entityManager, entity, true);
                        // Update live map
                        EdgeWallSlot(hWalls, vWalls, lwid->cellId, lwid->dir, lwid->half) = true;
                    }
                    if (lwid->warning)
                    {
                        lwid->warning = false;
                    }
                    continue;
                }

                // edgeState == Interior: both cells alive, normal random toggle.
                if (lwid->timer > 0.0f) continue;

                bool newEnabled = !lwid->enabled;

                // Speculatively apply to live map
                bool& slot = EdgeWallSlot(hWalls, vWalls, lwid->cellId, lwid->dir, lwid->half);
                slot = newEnabled;

                bool valid = true;
                if (newEnabled)
                    valid = !WouldSealSubtile(cx, cy, lwid->dir, hWalls, vWalls, cWallsMap, activeTiles);
                if (valid)
                    valid = IsMapValid(activeTiles, hWalls, vWalls, cWallsMap);

                if (valid)
                {
                    // Accept — live map already updated above
                    lwid->enabled = newEnabled;
                    lwid->timer = RandomTimer();
                    SyncCollider(entityManager, entity, newEnabled);
                    if (lwid->warning)
                    {
                        lwid->warning = false;
                    }
                }
                else
                {
                    // Reject — revert live map
                    slot = lwid->enabled;
                    if (lwid->warning)
                    {
                        lwid->warning = false;
                    }
                    lwid->timer = RandomTimer();
                }
            }
        }

        // Step 4: center spokes
        {
            auto spokeQuery = entityManager.CreateQuery<LaserWallID, CenterSpoke>();
            for (auto [entity, lwid, spoke] : spokeQuery)
            {
                // Like the walls above: a spoke whose tile was destroyed is forced off (collider too) regardless of its
                // timer, or it could be left mid-cycle with an active collider under a hidden mesh.
                if (!activeCellIds.count(lwid->cellId))
                {
                    if (lwid->enabled)
                    {
                        lwid->enabled = false;
                        SyncCollider(entityManager, entity, false);
                    }
                    if (lwid->warning) lwid->warning = false;
                    continue;
                }

                if (lwid->timer > 0.0f) continue;

                int cx = lwid->cellId / y_size;
                int cy = lwid->cellId % y_size;
                bool newEnabled = !lwid->enabled;

                // Speculatively update live cWallsMap
                bool& slot = cWallsMap[cx][cy][SpokeIndex(lwid->dir)];
                slot = newEnabled;

                if (newEnabled &&
                    (!IsCellSubtileConnected(cx, cy, hWalls, vWalls, cWallsMap, activeTiles) ||
                     !IsMapValid(activeTiles, hWalls, vWalls, cWallsMap)))
                {
                    // Reject — revert live cWallsMap
                    slot = false;
                    if (lwid->warning)
                    {
                        lwid->warning = false;
                    }
                    lwid->timer = RandomTimer();
                    continue;
                }

                lwid->enabled = newEnabled;
                lwid->timer = RandomTimer();
                SyncCollider(entityManager, entity, newEnabled);
                if (lwid->warning)
                {
                    lwid->warning = false;
                }
            }
        }

        // Step 5: tile destruction
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
            {
                EmitDestroyTile(events, pendingDestroyTileId);
                initialValidationDone = false;
            }
            tileDestroyTimer = TILE_DESTROY_INTERVAL;
            tileWarningEmitted = false;
            pendingDestroyTileId = -1;
        }

        // Debug print
        debugPrintArenaTimer -= deltaTime;
        if (debugPrintArenaTimer <= 0.0f)
        {
            PrintArenaState(entityManager);
            debugPrintArenaTimer = 10.0f;
        }

        // Wire codes from this tick's final warning/on-frame (Steps 3-4 clear the warning of walls that just toggled).
        {
            auto wallQuery = entityManager.CreateQuery<LaserWallID>();
            for (auto [entity, lwid] : wallQuery)
            {
                if (!lwid->warning) lwid->onFrame = -1;
                lwid->warnCode = EncodeWallWarning(lwid->warning, lwid->onFrame);
            }
        }
    }
};
