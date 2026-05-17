#pragma once

#include "netcode/netcode_common.hpp"
#include "ecs/ecs_common.hpp"
#include "Components.hpp"
#include "GameState.hpp"
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

    const float WARNING_THRESHOLD = 3.0f; // seconds before toggle to show warning

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
        bool vWalls[2 * MAP_SIZE][2 * MAP_SIZE + 1])
    {
        std::memset(hWalls, 0, sizeof(bool) * (2 * MAP_SIZE + 1) * (2 * MAP_SIZE));
        std::memset(vWalls, 0, sizeof(bool) * (2 * MAP_SIZE) * (2 * MAP_SIZE + 1));

        auto wallQuery = entityManager.CreateQuery<LaserWallID>();
        for (auto [entity, lwid] : wallQuery)
        {
            if (spokeEntities.count(entity)) continue;
            if (!lwid->enabled) continue;

            int cx = lwid->cellId / y_size;
            int cy = lwid->cellId % y_size;

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

    bool IsMapValid(
        const std::vector<std::pair<int, int>>& activeTiles,
        const bool hWalls[2 * MAP_SIZE + 1][2 * MAP_SIZE],
        const bool vWalls[2 * MAP_SIZE][2 * MAP_SIZE + 1])
    {
        if (activeTiles.empty()) return true;

        bool tileExists[MAP_SIZE][MAP_SIZE] = {};
        for (auto& t : activeTiles)
            tileExists[t.first][t.second] = true;

        bool visited[MAP_SIZE][MAP_SIZE] = {};
        std::queue<std::pair<int, int>> q;
        q.push(activeTiles[0]);
        visited[activeTiles[0].first][activeTiles[0].second] = true;
        int count = 1;

        while (!q.empty())
        {
            auto [cx, cy] = q.front(); q.pop();

            auto tryMove = [&](int nx, int ny, bool blocked)
                {
                    if (nx < 0 || nx >= x_size || ny < 0 || ny >= y_size) return;
                    if (blocked || visited[nx][ny]) return;
                    if (!tileExists[nx][ny]) return;
                    visited[nx][ny] = true;
                    count++;
                    q.push({ nx, ny });
                };

            tryMove(cx + 1, cy, hWalls[2 * cx + 2][2 * cy]);
            tryMove(cx - 1, cy, hWalls[2 * cx][2 * cy]);
            tryMove(cx, cy + 1, vWalls[2 * cx][2 * cy + 2]);
            tryMove(cx, cy - 1, vWalls[2 * cx][2 * cy]);
        }

        return count == (int)activeTiles.size();
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
                activeTiles.push_back({ tileId->id / y_size, tileId->id % y_size });
        }

        // ── Step 1: update all timers and warning flags ───────────────────────
        {
            auto wallQuery = entityManager.CreateQuery<LaserWallID>();
            for (auto [entity, lwid] : wallQuery)
            {
                lwid->timer -= deltaTime;

                // Warning: only for non-border walls that are about to toggle
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
                    lwid->warning = false; // border walls never warn
                }
            }
        }

        // ── Step 2: build wall map ────────────────────────────────────────────
        bool hWalls[2 * MAP_SIZE + 1][2 * MAP_SIZE];
        bool vWalls[2 * MAP_SIZE][2 * MAP_SIZE + 1];
        BuildWallMap(entityManager, spokeEntities, hWalls, vWalls);

        // ── Step 3: process expired non-spoke walls ───────────────────────────
        {
            auto wallQuery = entityManager.CreateQuery<LaserWallID>();
            for (auto [entity, lwid] : wallQuery)
            {
                if (spokeEntities.count(entity)) continue;
                if (lwid->timer > 0.0f) continue;

                int cx = lwid->cellId / y_size;
                int cy = lwid->cellId % y_size;

                if (IsBorderWall(lwid->cellId, lwid->dir, activeTiles))
                {
                    if (!lwid->enabled)
                    {
                        lwid->enabled = true;
                        EmitToggleWall(events, lwid->cellId, lwid->dir, false, true, cx, cy);
                    }
                    lwid->timer = RandomTimer();
                    lwid->warning = false;
                    continue;
                }

                bool newEnabled = !lwid->enabled;

                switch (lwid->dir)
                {
                case CellCardinalDirection::Down:  vWalls[2 * cx][2 * cy] = newEnabled; break;
                case CellCardinalDirection::Up:    vWalls[2 * cx][2 * cy + 2] = newEnabled; break;
                case CellCardinalDirection::Left:  hWalls[2 * cx][2 * cy] = newEnabled; break;
                case CellCardinalDirection::Right: hWalls[2 * cx + 2][2 * cy] = newEnabled; break;
                default: break;
                }

                if (IsMapValid(activeTiles, hWalls, vWalls))
                {
                    lwid->enabled = newEnabled;
                    lwid->timer = RandomTimer();
                    lwid->warning = false;
                    EmitToggleWall(events, lwid->cellId, lwid->dir, false, newEnabled, cx, cy);
                }
                else
                {
                    switch (lwid->dir)
                    {
                    case CellCardinalDirection::Down:  vWalls[2 * cx][2 * cy] = lwid->enabled; break;
                    case CellCardinalDirection::Up:    vWalls[2 * cx][2 * cy + 2] = lwid->enabled; break;
                    case CellCardinalDirection::Left:  hWalls[2 * cx][2 * cy] = lwid->enabled; break;
                    case CellCardinalDirection::Right: hWalls[2 * cx + 2][2 * cy] = lwid->enabled; break;
                    default: break;
                    }
                    lwid->timer = RandomTimer();
                    lwid->warning = false;
                }
            }
        }

        // ── Step 4: center spokes ─────────────────────────────────────────────
        {
            auto spokeQuery = entityManager.CreateQuery<LaserWallID, CenterSpoke>();
            for (auto [entity, lwid, spoke] : spokeQuery)
            {
                if (lwid->timer > 0.0f) continue;

                int cx = lwid->cellId / y_size;
                int cy = lwid->cellId % y_size;
                bool newEnabled = !lwid->enabled;
                lwid->enabled = newEnabled;
                lwid->timer = RandomTimer();
                lwid->warning = false;
                EmitToggleWall(events, lwid->cellId, lwid->dir, true, newEnabled, cx, cy);
            }
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