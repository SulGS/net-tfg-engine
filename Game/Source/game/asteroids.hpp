#ifndef ASTEROIDS
#define ASTEROIDS

#include "OpenGL/OpenGLIncludes.hpp"
#include "netcode/netcode_common.hpp"
#include <memory>
#include <cstring>
#include "Utils/Input.hpp"
#include "OpenGL/IGameRenderer.hpp"
#include <math.h>
#include <cmath>
#include "ecs/ecs.hpp"
#include "ecs/ecs_gamelogic.hpp"
#include "ecs/ecs_common.hpp"
#include "OpenGL/IECSGameRenderer.hpp"
#include "ecs/Collisions/BoxCollider2D.hpp"
#include "ecs/UI/UIButton.hpp"
#include "ecs/UI/UIImage.hpp"
#include "ecs/UI/UIText.hpp"
#include "ecs/UI/UIElement.hpp"

#include "GameState.hpp"
#include "Components.hpp"
#include "LogicSystems.hpp"
#include "RenderSystems.hpp"
#include "Events.hpp"
#include "EventHandlers.hpp"
#include "Deltas.hpp"
#include "DeltaHandler.hpp"
#include <unordered_set>

#include "OpenAL/AudioManager.hpp"
#include "OpenAL/AudioComponents.hpp"

#include <openssl/evp.h>




// Returns the world-space centre of tile (cx, cy).
// Matches the formula used in InitECSLogic / InitECSRenderer.
// cx, cy are subtile coords in the 10x10 grid (each original tile split into 2x2)
inline void SubTileCenterWorld(int cx, int cy, float& outX, float& outY)
{
    constexpr int x_size = 10;
    constexpr int y_size = 10;
    outX = (cx - x_size / 2.0f) * 40.0f - 20.0f;
    outY = (cy - y_size / 2.0f) * 40.0f - 20.0f;
}

// Canonical per-player spawn tiles and facing angles for up to MAX_PLAYERS players.
// Tiles are spread around the perimeter / corners of the 5x5 grid so players start
// far apart.  The facing angle (degrees, CCW from +X) points toward the arena centre.
struct SpawnPoint { int cx; int cy; float facingDeg; };

inline SpawnPoint GetSpawnPoint(int playerIndex)
{
    // 12 well-separated spawn points on the 10x10 grid (0-indexed cell coords).
    // Ordered counter-clockwise starting from bottom-left (0,0).
    // Facing angle points roughly toward the centre cell (4.5, 4.5).
    static const SpawnPoint table[NUM_PLAYERS] = {
        { 0, 0,  45.0f },   // bottom-left corner        → face NE
        { 3, 0,  83.0f },   // bottom edge, 1/3          → face N-NE
        {6, 0,  97.0f},   // bottom edge, 2/3          → face N-NW
        /*{ 9, 0, 135.0f },   // bottom-right corner       → face NW
        { 9, 3, 153.0f },   // right edge, 1/3           → face W-NW
        { 9, 6, 167.0f },   // right edge, 2/3           → face W-SW
        { 9, 9, 225.0f },   // top-right corner          → face SW
        { 6, 9, 263.0f },   // top edge, 2/3             → face S-SW
        { 3, 9, 277.0f },   // top edge, 1/3             → face S-SE
        { 0, 9, 315.0f },   // top-left corner           → face SE
        { 0, 6, 333.0f },   // left edge, 2/3            → face E-SE  (going down)
        { 0, 3, 347.0f },   // left edge, 1/3            → face E-NE  (going down)*/
    };

    if (playerIndex < 0 || playerIndex >= NUM_PLAYERS)
        return table[0];  // fallback
    return table[playerIndex];
}

static void GetPlayerSpawnWorld(int playerIndex, float& outX, float& outY, float& outRot)
{
    SpawnPoint sp = GetSpawnPoint(playerIndex);
    SubTileCenterWorld(sp.cx, sp.cy, outX, outY);
    outRot = sp.facingDeg;
}

class AsteroidShooterGame : public IECSGameLogic {
private:
    int debugTicks = 0;
public:

    void printGameState(const AsteroidShooterGameState& state) const
    {
        Debug::Info("GameState") << "=== Asteroid Shooter Game State ===\n";
        for (int i = 0; i < NUM_PLAYERS; i++) {
            Debug::Info("GameState") << "  Player " << i << ":\n";
            Debug::Info("GameState") << "    Position: (" << state.posX[i] << ", " << state.posY[i] << ")\n";
            Debug::Info("GameState") << "    Rotation: " << state.rot[i] << " degrees\n";
            Debug::Info("GameState") << "    Health: " << state.health[i] << "\n";
            Debug::Info("GameState") << "    Alive: " << (state.alive[i] ? "true" : "false") << "\n";
            Debug::Info("GameState") << "    Spectating: " << (!state.alive[i] ? "true" : "false") << "\n";
            Debug::Info("GameState") << "    Is Moving: " << (state.isMovingForward[i] ? "true" : "false") << "\n";
            Debug::Info("GameState") << "    Remaining shoot frames: " << state.remaingShootFrames[i] << "\n";
            Debug::Info("GameState") << "    Shoot Cooldown: " << state.shootCooldown[i] << "\n";
        }
        Debug::Info("GameState") << "  Active Bullet Count: " << state.bulletCount << "\n";
        Debug::Info("GameState") << "===================================\n";
    }

    std::unique_ptr<IGameLogic> Clone() const override {
        return std::make_unique<AsteroidShooterGame>();
    }

    InputBlob GenerateLocalInput() override {
        uint8_t m = INPUT_NONE;
        if (Input::KeyPressed(Input::CharToKeycode('a'))) m |= INPUT_LEFT;
        if (Input::KeyPressed(Input::CharToKeycode('d'))) m |= INPUT_RIGHT;
        if (Input::KeyPressed(Input::CharToKeycode('w'))) m |= INPUT_TOP;
        if (Input::KeyPressed(Input::CharToKeycode(' '))) m |= INPUT_SHOOT;
        InputBlob buf = MakeZeroInputBlob();
        buf.data[0] = m;
        return buf;
    }

    void GameState_To_ECSWorld(const GameStateBlob& state) {
        AsteroidShooterGameState s = *reinterpret_cast<const AsteroidShooterGameState*>(state.data);

        auto query = world.GetEntityManager().CreateQuery<Transform, Playable, SpaceShip>();
        for (auto [entity, transform, play, ship] : query) {
            int p = play->playerId;
            transform->setPosition(glm::vec3(s.posX[p], s.posY[p], 0.0f));
            transform->setRotation(glm::vec3(0.0f, 0.0f, s.rot[p]));
            ship->shipZRotation = s.rot[p];
            ship->shootCooldown = s.shootCooldown[p];
            ship->health = s.health[p];
            ship->isShooting = s.isShooting[p];
            ship->isMovingForward = s.isMovingForward[p];
            ship->shipInclination = s.shipInclination[p];
            ship->remainingShootFrames = s.remaingShootFrames[p];
            ship->isAlive = s.alive[p];
            ship->velX = s.velX[p];
            ship->velY = s.velY[p];
            ship->angularVel = s.angularVel[p];
        }

        // Collect active bullet IDs from ECS
        std::set<int> ecsActiveBulletIds;
        auto bulletQuery = world.GetEntityManager().CreateQuery<ECSBullet>();
        for (auto [entity, ecsb] : bulletQuery) {
            ecsActiveBulletIds.insert(ecsb->id);
        }

        // Collect active bullet IDs from state
        std::set<int> stateActiveBulletIds;
        for (int i = 0; i < MAX_BULLETS; i++) {
            if (s.bullets[i].active) {
                stateActiveBulletIds.insert(s.bullets[i].id);
            }
        }

        // Remove bullets not in state
        auto bulletQuery2 = world.GetEntityManager().CreateQuery<ECSBullet>();
        for (auto [entity, ecsb] : bulletQuery2) {
            if (stateActiveBulletIds.find(ecsb->id) == stateActiveBulletIds.end()) {
                world.GetEntityManager().DestroyEntity(entity);
            }
        }

        // Update or create bullets
        for (int i = 0; i < MAX_BULLETS; i++) {
            const Bullet& b = s.bullets[i];
            if (b.active) {
                bool found = false;
                auto bulletQuery3 = world.GetEntityManager().CreateQuery<Transform, ECSBullet>();
                for (auto [entity, transform, ecsb] : bulletQuery3) {
                    if (ecsb->id == b.id) {
                        transform->setPosition(glm::vec3(b.posX, b.posY, 0.0f));
                        ecsb->velX = b.velX;
                        ecsb->velY = b.velY;
                        ecsb->ownerId = b.ownerId;
                        ecsb->lifetime = b.lifetime;
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    Entity newBullet = world.GetEntityManager().CreateEntity();
                    Transform* t = world.GetEntityManager().AddComponent<Transform>(newBullet);
                    t->setPosition(glm::vec3(b.posX, b.posY, 0.0f));
                    world.GetEntityManager().AddComponent<ECSBullet>(newBullet, ECSBullet{ b.id, b.velX, b.velY, b.ownerId, b.lifetime });
                }
            }
        }

        const int x_size = 5;
        const int y_size = 5;

        // Sync tile active + warning flags from blob into logic ECS
        {
            auto tileQuery = world.GetEntityManager().CreateQuery<TileID>();
            for (auto [entity, tileId] : tileQuery)
            {
                int cx = tileId->id / y_size;
                int cy = tileId->id % y_size;
                tileId->active = s.tilesActive[cx][cy];
                tileId->warning = s.tilesWarning[cx][cy];
            }
        }
    }

    void ECSWorld_To_GameState(GameStateBlob& state) {
        AsteroidShooterGameState& s = *reinterpret_cast<AsteroidShooterGameState*>(state.data);

        std::memset(&s, 0, sizeof(s));
        s.bulletCount = 0;

        auto query = world.GetEntityManager().CreateQuery<Transform, Playable, SpaceShip>();
        for (auto [entity, transform, play, ship] : query) {
            int p = play->playerId;
            s.posX[p] = transform->getPosition().x;
            s.posY[p] = transform->getPosition().y;
            s.rot[p] = transform->getRotation().z;
            s.shootCooldown[p] = ship->shootCooldown;
            s.remaingShootFrames[p] = ship->remainingShootFrames;
            s.isShooting[p] = ship->isShooting;
            s.isMovingForward[p] = ship->isMovingForward;
            s.shipInclination[p] = ship->shipInclination;
            s.health[p] = ship->health;
            s.alive[p] = ship->isAlive;
            s.velX[p] = ship->velX;
            s.velY[p] = ship->velY;
            s.angularVel[p] = ship->angularVel;
        }

        auto bulletQuery = world.GetEntityManager().CreateQuery<Transform, ECSBullet>();
        int i = 0;
        for (auto [entity, transform, ecsb] : bulletQuery) {
            if (s.bulletCount < MAX_BULLETS) {
                Bullet& b = s.bullets[i];
                b.id = ecsb->id;
                b.posX = transform->getPosition().x;
                b.posY = transform->getPosition().y;
                b.velX = ecsb->velX;
                b.velY = ecsb->velY;
                b.ownerId = ecsb->ownerId;
                b.active = true;
                b.lifetime = ecsb->lifetime;
                s.bulletCount++;
                i++;
            }
        }

        const int x_size = 5;
        const int y_size = 5;

        // Tiles — active and warning
        {
            auto tileQuery = world.GetEntityManager().CreateQuery<TileID>();
            for (auto [entity, tileId] : tileQuery)
            {
                int cx = tileId->id / y_size;
                int cy = tileId->id % y_size;
                if (tileId->active)  s.tilesActive[cx][cy] = true;
                if (tileId->warning) s.tilesWarning[cx][cy] = true;
            }
        }

        // Border/shared walls — enabled and warning
        {
            auto wallQuery = world.GetEntityManager().CreateQuery<LaserWallID>();
            for (auto [entity, lwid] : wallQuery)
            {
                if (world.GetEntityManager().GetComponent<CenterSpoke>(entity) != nullptr) continue;

                int cx = lwid->cellId / y_size;
                int cy = lwid->cellId % y_size;

                switch (lwid->dir)
                {
                case CellCardinalDirection::Down:
                    s.vWalls[2 * cx][2 * cy] = lwid->enabled;
                    s.vWallsWarning[2 * cx][2 * cy] = lwid->warning;
                    break;
                case CellCardinalDirection::Up:
                    s.vWalls[2 * cx][2 * cy + 2] = lwid->enabled;
                    s.vWallsWarning[2 * cx][2 * cy + 2] = lwid->warning;
                    break;
                case CellCardinalDirection::Left:
                    s.hWalls[2 * cx][2 * cy] = lwid->enabled;
                    s.hWallsWarning[2 * cx][2 * cy] = lwid->warning;
                    break;
                case CellCardinalDirection::Right:
                    s.hWalls[2 * cx + 2][2 * cy] = lwid->enabled;
                    s.hWallsWarning[2 * cx + 2][2 * cy] = lwid->warning;
                    break;
                default: break;
                }
            }
        }

        // Center spokes — enabled and warning
        {
            auto spokeQuery = world.GetEntityManager().CreateQuery<LaserWallID, CenterSpoke>();
            for (auto [entity, lwid, spoke] : spokeQuery)
            {
                int cx = lwid->cellId / y_size;
                int cy = lwid->cellId % y_size;

                int dirIdx = 0;
                switch (lwid->dir)
                {
                case CellCardinalDirection::Down:  dirIdx = 0; break;
                case CellCardinalDirection::Up:    dirIdx = 1; break;
                case CellCardinalDirection::Left:  dirIdx = 2; break;
                case CellCardinalDirection::Right: dirIdx = 3; break;
                default: break;
                }
                s.cWalls[cx][cy][dirIdx] = lwid->enabled;
                s.cWallsWarning[cx][cy][dirIdx] = lwid->warning;
            }
        }

        state.len = sizeof(AsteroidShooterGameState);
    }

    bool CompareStates(const GameStateBlob& a, const GameStateBlob& b) const override {
        return std::memcmp(a.data, b.data, sizeof(AsteroidShooterGameState)) == 0;
    }

    void InitECSLogic(GameStateBlob& state) override {
        AsteroidShooterGameState* s = reinterpret_cast<AsteroidShooterGameState*>(state.data);

        float spawnX, spawnY, spawnRot;



        for (int i = 0; i < NUM_PLAYERS; i++) {
            GetPlayerSpawnWorld(i, spawnX, spawnY, spawnRot);
            s->posX[i] = spawnX;
            s->posY[i] = spawnY;
            s->rot[i] = spawnRot;

            s->velX[i] = 0.0f;
            s->velY[i] = 0.0f;
            s->angularVel[i] = 0.0f;

            s->remaingShootFrames[i] = 0;
            s->isShooting[i] = false;
            s->isMovingForward[i] = false;
            s->shipInclination[i] = 0;
            s->shootCooldown[i] = 0;
            s->health[i] = 1;
            s->alive[i] = true;
        }

        for (int i = 0; i < MAX_BULLETS; i++) {
            s->bullets[i].id = -1;
            s->bullets[i].active = false;
        }
        s->bulletCount = 0;

        state.len = sizeof(AsteroidShooterGameState);

        world.GetEntityManager().RegisterComponentType<SpaceShip>();
        world.GetEntityManager().RegisterComponentType<ECSBullet>();
        world.GetEntityManager().RegisterComponentType<TileID>();
        world.GetEntityManager().RegisterComponentType<PillarID>();
        world.GetEntityManager().RegisterComponentType<CenterSpoke>();
        world.GetEntityManager().RegisterComponentType<LaserWallID>();
        world.GetEntityManager().RegisterComponentType<SpectatorState>();


        for (int i = 0; i < NUM_PLAYERS; i++)
        {
            Entity player = world.GetEntityManager().CreateEntity();
            Transform* t = world.GetEntityManager().AddComponent<Transform>(player, Transform{});
            t->setPosition(glm::vec3(s->posX[i], s->posY[i], 0.0f));
            t->setRotation(glm::vec3(0.0f, 0.0f, s->rot[i]));
            world.GetEntityManager().AddComponent<Playable>(player, Playable{ i, MakeZeroInputBlob(), (i == playerId ? true : false) });
            world.GetEntityManager().AddComponent<SpaceShip>(player, SpaceShip{ 1, -1, 0, true });

            if (isServer)
            {
                BoxCollider2D* collider = world.GetEntityManager().AddComponent<BoxCollider2D>(player, BoxCollider2D{ glm::vec2(1.5f, 3.0f) });
                collider->layer = CollisionLayer::PLAYER;
                collider->collidesWith = CollisionLayer::BULLET;
            }
        }


        const int x_size = 5;
        const int y_size = 5;

        auto& em = world.GetEntityManager();

        // --- Tiles ---
        for (int x = 0; x < x_size; x++)
        {
            for (int y = 0; y < y_size; y++)
            {
                const int cellId = x * y_size + y;
                Entity e = em.CreateEntity();
                Transform* t = em.AddComponent<Transform>(e, Transform{});
                t->setPosition(glm::vec3((x - x_size / 2.0f) * 80.0f,
                    (y - y_size / 2.0f) * 80.0f, -4.0f));
                t->setScale(glm::vec3(1.0f));
                em.AddComponent<TileID>(e, TileID{ cellId });
            }
        }

        // --- Pillars (logic side — no mesh) ---
        for (int px = 0; px <= 2 * x_size; px++)
        {
            for (int py = 0; py <= 2 * y_size; py++)
            {
                const bool isTileCentre = (px % 2 == 1 && py % 2 == 1);

                Entity e = em.CreateEntity();
                Transform* t = em.AddComponent<Transform>(e, Transform{});
                t->setPosition(glm::vec3((px - x_size) * 40.0f - 40.0f,
                    (py - y_size) * 40.0f - 40.0f, -4.0f));
                t->setRotation(glm::vec3(90.0f, 0.0f, 0.0f));
                t->setScale(glm::vec3(1.0f));

                PillarID pid;
                if (isTileCentre)
                {
                    const int cx = (px - 1) / 2;
                    const int cy = (py - 1) / 2;
                    pid.addCell(cx * y_size + cy);
                }
                else
                {
                    const int cx_min = (px - 2) / 2;
                    const int cx_max = px / 2;
                    const int cy_min = (py - 2) / 2;
                    const int cy_max = py / 2;

                    for (int cx = std::max(cx_min, 0); cx <= std::min(cx_max, x_size - 1); cx++)
                    {
                        for (int cy = std::max(cy_min, 0); cy <= std::min(cy_max, y_size - 1); cy++)
                        {
                            const bool onBorderX = (px == 2 * cx || px == 2 * cx + 1 || px == 2 * cx + 2);
                            const bool onBorderY = (py == 2 * cy || py == 2 * cy + 1 || py == 2 * cy + 2);
                            if (onBorderX && onBorderY)
                                pid.addCell(cx * y_size + cy);
                        }
                    }
                }
                em.AddComponent<PillarID>(e, pid);
            }
        }

        // --- Walls ---
        std::mt19937 initRng{ std::random_device{}() };
        std::uniform_real_distribution<float> initDist(3.0f, 30.0f);

        std::vector<WallDef> walls;

        // Walls between horizontally adjacent pillars (px,py)→(px+1,py)
        for (int px = 0; px < 2 * x_size; px++)
        {
            for (int py = 0; py <= 2 * y_size; py++)
            {
                const float midX = ((px - x_size) * 40.0f - 40.0f + (px + 1 - x_size) * 40.0f - 40.0f) / 2.0f;
                const float midY = (py - y_size) * 40.0f - 40.0f;

                for (int cx = 0; cx < x_size; cx++)
                {
                    for (int cy = 0; cy < y_size; cy++)
                    {
                        if (px < 2 * cx || px >= 2 * cx + 2) continue;
                        if (py != 2 * cy && py != 2 * cy + 2) continue;

                        const CellCardinalDirection dir = (py == 2 * cy)
                            ? CellCardinalDirection::Down
                            : CellCardinalDirection::Up;
                        const bool onBorder = (py == 0) || (py == 2 * y_size);

                        walls.push_back({ cx, cy, dir,
                            glm::vec3(midX, midY, 0.0f),
                            glm::vec3(0.0f, 90.0f, 0.0f),
                            onBorder });
                    }
                }
            }
        }

        // Walls between vertically adjacent pillars (px,py)→(px,py+1)
        for (int px = 0; px <= 2 * x_size; px++)
        {
            for (int py = 0; py < 2 * y_size; py++)
            {
                const float midX = (px - x_size) * 40.0f - 40.0f;
                const float midY = ((py - y_size) * 40.0f - 40.0f + (py + 1 - y_size) * 40.0f - 40.0f) / 2.0f;

                for (int cx = 0; cx < x_size; cx++)
                {
                    for (int cy = 0; cy < y_size; cy++)
                    {
                        if (py < 2 * cy || py >= 2 * cy + 2) continue;
                        if (px != 2 * cx && px != 2 * cx + 2) continue;

                        const CellCardinalDirection dir = (px == 2 * cx)
                            ? CellCardinalDirection::Left
                            : CellCardinalDirection::Right;
                        const bool onBorder = (px == 0) || (px == 2 * x_size);

                        walls.push_back({ cx, cy, dir,
                            glm::vec3(midX, midY, 0.0f),
                            glm::vec3(90.0f, 0.0f, 0.0f),
                            onBorder });
                    }
                }
            }
        }

        // Spawn border/shared wall entities
        for (auto& w : walls)
        {
            const int cellId = w.cellX * y_size + w.cellY;

            Entity e = em.CreateEntity();
            Transform* t = em.AddComponent<Transform>(e, Transform{});
            t->setPosition(w.pos);
            t->setRotation(w.rot);
            t->setScale(glm::vec3(2.0f, 2.0f, 19.0f));

            LaserWallID lwid(cellId, w.dir);
            lwid.enabled = w.onBorder;
            lwid.timer = initDist(initRng);
            em.AddComponent<LaserWallID>(e, lwid);
        }

        // Center spokes
        for (int cx = 0; cx < x_size; cx++)
        {
            for (int cy = 0; cy < y_size; cy++)
            {
                const float centerX = (2 * cx + 1 - x_size) * 40.0f - 40.0f;
                const float centerY = (2 * cy + 1 - y_size) * 40.0f - 40.0f;

                auto makeSpoke = [&](CellCardinalDirection dir, glm::vec3 pos, glm::vec3 rot)
                    {
                        const int cellId = cx * y_size + cy;
                        Entity e = em.CreateEntity();
                        Transform* t = em.AddComponent<Transform>(e, Transform{});
                        t->setPosition(pos);
                        t->setRotation(rot);
                        t->setScale(glm::vec3(2.0f, 2.0f, 19.0f));

                        LaserWallID lwid(cellId, dir);
                        lwid.enabled = false;
                        lwid.timer = initDist(initRng);
                        em.AddComponent<LaserWallID>(e, lwid);
                        em.AddComponent<CenterSpoke>(e, CenterSpoke{});
                    };

                {
                    const float edgeY = (2 * cy - y_size) * 40.0f - 40.0f;
                    makeSpoke(CellCardinalDirection::Down,
                        glm::vec3(centerX, (centerY + edgeY) / 2.0f, 0.0f),
                        glm::vec3(90.0f, 0.0f, 0.0f));
                }
                {
                    const float edgeY = (2 * cy + 2 - y_size) * 40.0f - 40.0f;
                    makeSpoke(CellCardinalDirection::Up,
                        glm::vec3(centerX, (centerY + edgeY) / 2.0f, 0.0f),
                        glm::vec3(90.0f, 0.0f, 0.0f));
                }
                {
                    const float edgeX = (2 * cx - x_size) * 40.0f - 40.0f;
                    makeSpoke(CellCardinalDirection::Left,
                        glm::vec3((centerX + edgeX) / 2.0f, centerY, 0.0f),
                        glm::vec3(0.0f, 90.0f, 0.0f));
                }
                {
                    const float edgeX = (2 * cx + 2 - x_size) * 40.0f - 40.0f;
                    makeSpoke(CellCardinalDirection::Right,
                        glm::vec3((centerX + edgeX) / 2.0f, centerY, 0.0f),
                        glm::vec3(0.0f, 90.0f, 0.0f));
                }
            }
        }


        world.AddSystem(std::make_unique<InputSystem>());
        if (isServer)
        {
            world.AddSystem(std::make_unique<InputServerSystem>());
            world.AddSystem(std::make_unique<ArenaSystem>());
            world.AddSystem(std::make_unique<GameOverSystem>());
        }

        world.AddSystem(std::make_unique<BulletSystem>());
        world.AddSystem(std::make_unique<OnDeathLogicSystem>());

        eventProcessor->RegisterHandler(AsteroidEventMask::SPAWN_BULLET, std::make_unique<SpawnBulletHandler>());
        eventProcessor->RegisterHandler(AsteroidEventMask::BULLET_COLLIDES, std::make_unique<BulletCollidesHandler>());
        eventProcessor->RegisterHandler(AsteroidEventMask::DEATH, std::make_unique<DeathHandler>());
        eventProcessor->RegisterHandler(AsteroidEventMask::DESTROY_TILE, std::make_unique<DestroyTileHandler>());
        eventProcessor->RegisterHandler(AsteroidEventMask::TOGGLE_WALL, std::make_unique<ToggleWallHandler>());
        eventProcessor->RegisterHandler(AsteroidEventMask::WARN_TILE, std::make_unique<WarnTileHandler>());
        eventProcessor->RegisterHandler(AsteroidEventMask::WARN_WALL, std::make_unique<WarnWallHandler>());

        deltaProcessor->RegisterHandler(DELTA_GAME_POSITIONS, std::make_unique<GamePositionsDeltaHandler>());
    }

    void HashState(const GameStateBlob& state, uint8_t(&outHash)[SHA256_DIGEST_LENGTH]) const override {
        const AsteroidShooterGameState& s =
            *reinterpret_cast<const AsteroidShooterGameState*>(state.data);

        EVP_MD_CTX* ctx = EVP_MD_CTX_new();
        if (!ctx) throw std::runtime_error("Failed to create EVP_MD_CTX");

        if (1 != EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr)) {
            EVP_MD_CTX_free(ctx);
            throw std::runtime_error("EVP_DigestInit_ex failed");
        }

        EVP_DigestUpdate(ctx, &s.health[0], sizeof(s.health[0]));
        EVP_DigestUpdate(ctx, &s.health[1], sizeof(s.health[1]));
        EVP_DigestUpdate(ctx, &s.alive[0], sizeof(s.alive[0]));
        EVP_DigestUpdate(ctx, &s.alive[1], sizeof(s.alive[1]));

        unsigned int len = 0;
        if (1 != EVP_DigestFinal_ex(ctx, outHash, &len)) {
            EVP_MD_CTX_free(ctx);
            throw std::runtime_error("EVP_DigestFinal_ex failed");
        }
        EVP_MD_CTX_free(ctx);
    }

    void PrintState(const GameStateBlob& state) const override {
        AsteroidShooterGameState s;
        std::memcpy(&s, state.data, sizeof(AsteroidShooterGameState));
        printGameState(s);
    }
};




class AsteroidShooterGameRenderer : public IECSGameRenderer {
public:

    void printGameState(const AsteroidShooterGameState& state) const
    {
        Debug::Info("GameState") << "=== Asteroid Shooter Game State ===\n";
        for (int i = 0; i < 2; i++) {
            Debug::Info("GameState") << "  Player " << i << ":\n";
            Debug::Info("GameState") << "    Position: (" << state.posX[i] << ", " << state.posY[i] << ")\n";
            Debug::Info("GameState") << "    Rotation: " << state.rot[i] << " degrees\n";
            Debug::Info("GameState") << "    Health: " << state.health[i] << "\n";
            Debug::Info("GameState") << "    Alive: " << (state.alive[i] ? "true" : "false") << "\n";
            Debug::Info("GameState") << "    Spectating: " << (!state.alive[i] ? "true" : "false") << "\n";
            Debug::Info("GameState") << "    Remaining shoot frames: " << state.remaingShootFrames[i] << "\n";
            Debug::Info("GameState") << "    Shoot Cooldown: " << state.shootCooldown[i] << "\n";
        }
        Debug::Info("GameState") << "  Active Bullet Count: " << state.bulletCount << "\n";
        Debug::Info("GameState") << "===================================\n";
    }

    void GameState_To_ECSWorld(const GameStateBlob& state)
    {
        AsteroidShooterGameState s =
            *reinterpret_cast<const AsteroidShooterGameState*>(state.data);

        EntityManager& em = world.GetEntityManager();

        // Players
        auto playerQuery = em.CreateQuery<Transform, Playable, SpaceShip>();
        for (auto [entity, transform, play, ship] : playerQuery)
        {
            int p = play->playerId;
            transform->setPosition(glm::vec3(s.posX[p], s.posY[p], 0.0f));
            transform->setRotation(glm::vec3(0.0f, 0.0f, s.rot[p]));
            ship->health = s.health[p];
            ship->isAlive = s.alive[p];
            ship->shipZRotation = s.rot[p];
            ship->isShooting = s.isShooting[p];
            ship->isMovingForward = s.isMovingForward[p];
            ship->shipInclination = s.shipInclination[p];
            ship->remainingShootFrames = s.remaingShootFrames[p];
            ship->shootCooldown = s.shootCooldown[p];
            ship->velX = s.velX[p];
            ship->velY = s.velY[p];
            ship->angularVel = s.angularVel[p];
        }

        // Bullets — collect ECS ids
        std::set<int> ecsActiveBulletIds;
        {
            auto q = em.CreateQuery<ECSBullet>();
            for (auto [e, ecsb] : q)
                ecsActiveBulletIds.insert(ecsb->id);
        }

        // Collect state ids
        std::set<int> stateActiveBulletIds;
        for (int i = 0; i < MAX_BULLETS; i++)
            if (s.bullets[i].active)
                stateActiveBulletIds.insert(s.bullets[i].id);

        // Remove stale bullets
        {
            auto q = em.CreateQuery<ECSBullet, MeshComponent>();
            for (auto [e, ecsb, mesh] : q)
                if (stateActiveBulletIds.find(ecsb->id) == stateActiveBulletIds.end())
                    em.DestroyEntity(e);
        }

        // Update or create bullets
        for (int i = 0; i < MAX_BULLETS; i++)
        {
            const Bullet& b = s.bullets[i];
            if (!b.active) continue;

            bool found = false;
            auto q = em.CreateQuery<Transform, ECSBullet, MeshComponent>();
            for (auto [e, transform, ecsb, mesh] : q)
            {
                if (ecsb->id == b.id)
                {
                    transform->setPosition(glm::vec3(b.posX, b.posY, 0.0f));
                    ecsb->velX = b.velX;
                    ecsb->velY = b.velY;
                    ecsb->ownerId = b.ownerId;
                    ecsb->lifetime = b.lifetime;
                    found = true;
                    break;
                }
            }

            if (!found)
            {
                Entity newBullet = em.CreateEntity();
                Transform* t = em.AddComponent<Transform>(newBullet, Transform{});
                t->setPosition(glm::vec3(b.posX, b.posY, 0.0f));

                auto bulletMat = std::make_shared<Material>("generic.vert", "generic.frag");
                bulletMat->setVec3("uColor", glm::vec3(1.0f, 1.0f, 0.0f));

                em.AddComponent<ECSBullet>(newBullet, ECSBullet{ b.id, b.velX, b.velY, b.ownerId, b.lifetime });
                em.AddComponent<MeshComponent>(newBullet, MeshComponent(new Mesh("bullet.glb", bulletMat)));

                Entity bulletSound = em.CreateEntity();
                Transform* st = em.AddComponent<Transform>(bulletSound, Transform{});
                st->setPosition(glm::vec3(b.posX, b.posY, 0.0f));
                DestroyTimer* dt = em.AddComponent<DestroyTimer>(bulletSound, DestroyTimer{});
                dt->framesRemaining = RENDER_TICKS_PER_SECOND * 3;
                AudioSourceComponent* audio = em.AddComponent<AudioSourceComponent>(
                    bulletSound, AudioSourceComponent("shoot.wav", AudioChannel::SFX, false));
                audio->play = true;
				audio->gain = 1.0f;
				LinkAudioToBullet* link = em.AddComponent<LinkAudioToBullet>(bulletSound, LinkAudioToBullet{ b.id });
            }
        }

        const int x_size = 5;
        const int y_size = 5;

        // Sync tile active + warning flags
        {
            auto tileQuery = em.CreateQuery<TileID>();
            for (auto [entity, tileId] : tileQuery)
            {
                int cx = tileId->id / y_size;
                int cy = tileId->id % y_size;
                tileId->active = s.tilesActive[cx][cy];
                tileId->warning = s.tilesWarning[cx][cy];
            }
        }

        // Sync wall enabled + warning state
        {
            auto wallQuery = em.CreateQuery<LaserWallID>();
            for (auto [entity, lwid] : wallQuery)
            {
                if (em.GetComponent<CenterSpoke>(entity) != nullptr) continue;
                int cx = lwid->cellId / y_size;
                int cy = lwid->cellId % y_size;
                switch (lwid->dir)
                {
                case CellCardinalDirection::Down:
                    lwid->enabled = s.vWalls[2 * cx][2 * cy];
                    lwid->warning = s.vWallsWarning[2 * cx][2 * cy];
                    break;
                case CellCardinalDirection::Up:
                    lwid->enabled = s.vWalls[2 * cx][2 * cy + 2];
                    lwid->warning = s.vWallsWarning[2 * cx][2 * cy + 2];
                    break;
                case CellCardinalDirection::Left:
                    lwid->enabled = s.hWalls[2 * cx][2 * cy];
                    lwid->warning = s.hWallsWarning[2 * cx][2 * cy];
                    break;
                case CellCardinalDirection::Right:
                    lwid->enabled = s.hWalls[2 * cx + 2][2 * cy];
                    lwid->warning = s.hWallsWarning[2 * cx + 2][2 * cy];
                    break;
                default: break;
                }
            }

            auto spokeQuery = em.CreateQuery<LaserWallID, CenterSpoke>();
            for (auto [entity, lwid, spoke] : spokeQuery)
            {
                int cx = lwid->cellId / y_size;
                int cy = lwid->cellId % y_size;
                int dirIdx = 0;
                switch (lwid->dir)
                {
                case CellCardinalDirection::Down:  dirIdx = 0; break;
                case CellCardinalDirection::Up:    dirIdx = 1; break;
                case CellCardinalDirection::Left:  dirIdx = 2; break;
                case CellCardinalDirection::Right: dirIdx = 3; break;
                default: break;
                }
                lwid->enabled = s.cWalls[cx][cy][dirIdx];
                lwid->warning = s.cWallsWarning[cx][cy][dirIdx];
            }
        }
    }

    void InitECSRenderer(const GameStateBlob& state, OpenGLWindow* window) override {
        this->window = window;

        AsteroidShooterGameState s;
        std::memcpy(&s, state.data, sizeof(AsteroidShooterGameState));

        world.GetEntityManager().RegisterComponentType<SpaceShip>();
        world.GetEntityManager().RegisterComponentType<ECSBullet>();
        world.GetEntityManager().RegisterComponentType<ChargingShootEffect>();
        world.GetEntityManager().RegisterComponentType<DestroyTimer>();
        world.GetEntityManager().RegisterComponentType<TileID>();
        world.GetEntityManager().RegisterComponentType<PillarID>();
        world.GetEntityManager().RegisterComponentType<LaserWallID>();
        world.GetEntityManager().RegisterComponentType<CenterSpoke>();
        world.GetEntityManager().RegisterComponentType<ThrusterOwner>();
        world.GetEntityManager().RegisterComponentType<SpectatorState>();
		world.GetEntityManager().RegisterComponentType<JustDeathChecker>();
		world.GetEntityManager().RegisterComponentType<ExplosionPlayerID>();
		world.GetEntityManager().RegisterComponentType<LinkAudioToBullet>();

        // --- Sun ---
        Entity sunEntity = world.GetEntityManager().CreateEntity();
        world.GetEntityManager().AddComponent<Transform>(sunEntity, Transform{});
        DirectionalLightComponent* sunLight =
            world.GetEntityManager().AddComponent<DirectionalLightComponent>(sunEntity, DirectionalLightComponent{});
        sunLight->color = glm::vec3(1.0f, 0.95f, 0.8f);

        // --- Players ---
        for (int i = 0; i < NUM_PLAYERS; ++i)
        {

            Entity player = world.GetEntityManager().CreateEntity();
            Transform* t = world.GetEntityManager().AddComponent<Transform>(player, Transform{});
            t->setPosition(glm::vec3(s.posX[i], s.posY[i], 0.0f));
            t->setRotation(glm::vec3(0.0f, 0.0f, s.rot[i]));
            t->setScale(glm::vec3(5.0f, 5.0f, 5.0f));

            world.GetEntityManager().AddComponent<Playable>(
                player, Playable{ i, MakeZeroInputBlob(), (i == playerId) });
            world.GetEntityManager().AddComponent<SpaceShip>(player, SpaceShip{ 1, -1, 0, true });
            world.GetEntityManager().AddComponent<MeshComponent>(
                player, MeshComponent(new Mesh("ship.glb",
                    std::make_shared<Material>("ggx.vert", "ggx.frag"))));

			world.GetEntityManager().AddComponent<JustDeathChecker>(player, JustDeathChecker{});

            if (i == playerId)
            {
				Entity listenerForShipEntity = world.GetEntityManager().CreateEntity();
                Transform* tL = world.GetEntityManager().AddComponent<Transform>(listenerForShipEntity, Transform{});
                tL->setPosition(glm::vec3(s.posX[i], s.posY[i], 0.0f));
                world.GetEntityManager().AddComponent<AudioListenerComponent>(listenerForShipEntity, AudioListenerComponent{});
            }

            // --- Thrusters for this player ---
            struct ThrusterDef { bool isSmoke; bool isLeft; };
            constexpr ThrusterDef thrusterDefs[] = {
                { false, false },  // right thruster
                { false, true  },  // left thruster
                { true,  true  },  // left smoke
                { true,  false },  // right smoke
            };

            for (const auto& def : thrusterDefs)
            {
                Entity thrusterEntity = world.GetEntityManager().CreateEntity();
                Transform* tt = world.GetEntityManager().AddComponent<Transform>(thrusterEntity, Transform{});
                tt->setPosition(glm::vec3(0.0f, 0.0f, 2.0f));
                tt->setRotation(glm::vec3(0.0f, 90.0f, 0.0f));
                tt->setScale(glm::vec3(8.0f, 8.0f, 8.0f));

                world.GetEntityManager().AddComponent<ThrusterOwner>(
                    thrusterEntity, ThrusterOwner{ i, def.isSmoke, def.isLeft });

                if (!def.isSmoke)
                {
                    auto* particle = world.GetEntityManager().AddComponent<ParticleEmitterComponent>(
                        thrusterEntity, ParticlePresets::SpaceshipThruster());
                    particle->emissionRate = 300.0f;
                    particle->startLifetime = 0.05f;
                }
                else
                {
                    auto* particle = world.GetEntityManager().AddComponent<ParticleEmitterComponent>(
                        thrusterEntity, ParticlePresets::Smoke());
                    particle->emissionRate = 6.0f;
                    particle->startLifetime = 2.5f;
                    particle->startSpeed = 0.8f;
                    particle->startSize = 0.15f;
                    particle->endSize = 0.5f;
                    particle->gravityModifier = 0.15f;
                    particle->startColor = glm::vec4(0.08f, 0.08f, 0.08f, 0.8f);
                    particle->endColor = glm::vec4(0.05f, 0.05f, 0.05f, 0.0f);
                    particle->shape = EmitterShape::Cone;
                    particle->shapeConeAngle = 0.2f;
                    particle->lifetimeVariance = 0.6f;
                    particle->turbulenceStrength = 0.1f;
                    particle->enabled = false;
                }
            }
        }

        // --- Camera ---
        Entity camera = world.GetEntityManager().CreateEntity();
        Transform* camTrans = world.GetEntityManager().AddComponent<Transform>(camera, Transform{});
        camTrans->setPosition(glm::vec3(0.0f, 0.0f, 18.0f));
        Camera* camSettings = world.GetEntityManager().AddComponent<Camera>(camera, Camera{});
        camSettings->setPerspective(45.0f, window->getAspectRatio(), 0.001f, 1000.0f);
        camSettings->setTarget(glm::vec3(0.0f, 0.0f, 0.0f));
        camSettings->setUp(glm::vec3(0.0f, 1.0f, 0.0f));

        // --- Health UI ---
        Entity healthText = world.GetEntityManager().CreateEntity();
        UIElement* element = world.GetEntityManager().AddComponent<UIElement>(healthText, UIElement{});
        element->anchor = UIAnchor::TOP_LEFT;
        element->position = glm::vec2(0.0f, 0.0f);
        element->size = glm::vec2(150.0f, 40.0f);
        element->pivot = glm::vec2(0.0f, 0.0f);
        element->layer = 1;
        UIText* text = world.GetEntityManager().AddComponent<UIText>(healthText, UIText{});
        text->text = "Health:";
        text->fontSize = 32.0f;
        text->SetColor(1.0f, 1.0f, 1.0f, 1.0f);
        text->SetFont("default");

        // --- Point light ---
        Entity light = world.GetEntityManager().CreateEntity();
        Transform* tlight = world.GetEntityManager().AddComponent<Transform>(light, Transform{});
        tlight->setPosition(glm::vec3(0.0f, 0.0f, 15.0f));
        PointLightComponent* lightComp =
            world.GetEntityManager().AddComponent<PointLightComponent>(light, PointLightComponent{});
        lightComp->color = glm::vec3(0.302f, 0.651f, 1.0f);
        lightComp->intensity = 1000.0f;
        lightComp->radius = 100.0f;
        lightComp->castShadows = true;

        // --- Lava floor ---
        Entity lavaFloor = world.GetEntityManager().CreateEntity();
        Transform* lavaTrans = world.GetEntityManager().AddComponent<Transform>(lavaFloor, Transform{});
        lavaTrans->setPosition(glm::vec3(-20.0f, -20.0f, -12.0f));
        lavaTrans->setRotation(glm::vec3(90.0f, 0.0f, 0.0f));
        lavaTrans->setScale(glm::vec3(5.0f, 5.0f, 5.0f));
        MeshComponent* lavaMesh = world.GetEntityManager().AddComponent<MeshComponent>(
            lavaFloor, MeshComponent(new Mesh("lava.glb",
                std::make_shared<Material>("ggx.vert", "ggx.frag"))));
        lavaMesh->castShadows = false;

        const int x_size = 5;
        const int y_size = 5;
        auto& em = world.GetEntityManager();

        // --- Tiles ---
        for (int x = 0; x < x_size; ++x)
            for (int y = 0; y < y_size; ++y)
            {
                const int cellId = x * y_size + y;
                Entity e = em.CreateEntity();
                Transform* t = em.AddComponent<Transform>(e, Transform{});
                t->setPosition(glm::vec3((x - x_size / 2.0f) * 80.0f,
                    (y - y_size / 2.0f) * 80.0f, -4.0f));
                t->setScale(glm::vec3(1.0f));
                em.AddComponent<TileID>(e, TileID{ cellId });
                em.AddComponent<MeshComponent>(e, MeshComponent(
                    new Mesh("tile.glb", std::make_shared<Material>("ggx.vert", "ggx.frag"))));
            }

        // --- Pillars ---
        for (int px = 0; px <= 2 * x_size; ++px)
            for (int py = 0; py <= 2 * y_size; ++py)
            {
                const bool isTileCentre = (px % 2 == 1 && py % 2 == 1);

                Entity e = em.CreateEntity();
                Transform* t = em.AddComponent<Transform>(e, Transform{});
                t->setPosition(glm::vec3((px - x_size) * 40.0f - 40.0f,
                    (py - y_size) * 40.0f - 40.0f, -4.0f));
                t->setRotation(glm::vec3(90.0f, 0.0f, 0.0f));
                t->setScale(glm::vec3(1.0f));

                PillarID pid;
                if (isTileCentre)
                {
                    const int cx = (px - 1) / 2;
                    const int cy = (py - 1) / 2;
                    pid.addCell(cx * y_size + cy);
                }
                else
                {
                    const int cx_min = (px - 2) / 2;
                    const int cx_max = px / 2;
                    const int cy_min = (py - 2) / 2;
                    const int cy_max = py / 2;
                    for (int cx = std::max(cx_min, 0); cx <= std::min(cx_max, x_size - 1); ++cx)
                        for (int cy = std::max(cy_min, 0); cy <= std::min(cy_max, y_size - 1); ++cy)
                        {
                            const bool onBorderX = (px == 2 * cx || px == 2 * cx + 1 || px == 2 * cx + 2);
                            const bool onBorderY = (py == 2 * cy || py == 2 * cy + 1 || py == 2 * cy + 2);
                            if (onBorderX && onBorderY)
                                pid.addCell(cx * y_size + cy);
                        }
                }
                em.AddComponent<PillarID>(e, pid);
                em.AddComponent<MeshComponent>(e, MeshComponent(
                    new Mesh("pilar.glb", std::make_shared<Material>("ggx.vert", "ggx.frag"))));
            }

        // --- Walls ---
        std::vector<WallDef> walls;

        for (int px = 0; px < 2 * x_size; ++px)
            for (int py = 0; py <= 2 * y_size; ++py)
            {
                const float midX = ((px - x_size) * 40.0f - 40.0f + (px + 1 - x_size) * 40.0f - 40.0f) / 2.0f;
                const float midY = (py - y_size) * 40.0f - 40.0f;
                for (int cx = 0; cx < x_size; ++cx)
                    for (int cy = 0; cy < y_size; ++cy)
                    {
                        if (px < 2 * cx || px >= 2 * cx + 2) continue;
                        if (py != 2 * cy && py != 2 * cy + 2) continue;
                        const CellCardinalDirection dir = (py == 2 * cy)
                            ? CellCardinalDirection::Down : CellCardinalDirection::Up;
                        const bool onBorder = (py == 0) || (py == 2 * y_size);
                        walls.push_back({ cx, cy, dir,
                            glm::vec3(midX, midY, 0.0f),
                            glm::vec3(0.0f, 90.0f, 0.0f), onBorder });
                    }
            }

        for (int px = 0; px <= 2 * x_size; ++px)
            for (int py = 0; py < 2 * y_size; ++py)
            {
                const float midX = (px - x_size) * 40.0f - 40.0f;
                const float midY = ((py - y_size) * 40.0f - 40.0f + (py + 1 - y_size) * 40.0f - 40.0f) / 2.0f;
                for (int cx = 0; cx < x_size; ++cx)
                    for (int cy = 0; cy < y_size; ++cy)
                    {
                        if (py < 2 * cy || py >= 2 * cy + 2) continue;
                        if (px != 2 * cx && px != 2 * cx + 2) continue;
                        const CellCardinalDirection dir = (px == 2 * cx)
                            ? CellCardinalDirection::Left : CellCardinalDirection::Right;
                        const bool onBorder = (px == 0) || (px == 2 * x_size);
                        walls.push_back({ cx, cy, dir,
                            glm::vec3(midX, midY, 0.0f),
                            glm::vec3(90.0f, 0.0f, 0.0f), onBorder });
                    }
            }

        for (auto& w : walls)
        {
            const int cellId = w.cellX * y_size + w.cellY;
            Entity e = em.CreateEntity();
            Transform* t = em.AddComponent<Transform>(e, Transform{});
            t->setPosition(w.pos);
            t->setRotation(w.rot);
            t->setScale(glm::vec3(2.0f, 2.0f, 19.0f));
            LaserWallID lwid(cellId, w.dir);
            lwid.enabled = w.onBorder;
            em.AddComponent<LaserWallID>(e, lwid);
            MeshComponent* mc = em.AddComponent<MeshComponent>(e,
                MeshComponent(new Mesh("laser_wall.glb",
                    std::make_shared<Material>("ggx.vert", "ggx.frag"))));
            mc->castShadows = false;
        }

        // --- Center spokes ---
        for (int cx = 0; cx < x_size; ++cx)
            for (int cy = 0; cy < y_size; ++cy)
            {
                const float centerX = (2 * cx + 1 - x_size) * 40.0f - 40.0f;
                const float centerY = (2 * cy + 1 - y_size) * 40.0f - 40.0f;
                const int cellId = cx * y_size + cy;

                auto makeSpoke = [&](CellCardinalDirection dir, glm::vec3 pos, glm::vec3 rot)
                    {
                        Entity e = em.CreateEntity();
                        Transform* t = em.AddComponent<Transform>(e, Transform{});
                        t->setPosition(pos);
                        t->setRotation(rot);
                        t->setScale(glm::vec3(2.0f, 2.0f, 19.0f));
                        LaserWallID lwid(cellId, dir);
                        lwid.enabled = false;
                        em.AddComponent<LaserWallID>(e, lwid);
                        em.AddComponent<CenterSpoke>(e, CenterSpoke{});
                        MeshComponent* mc = em.AddComponent<MeshComponent>(e,
                            MeshComponent(new Mesh("laser_wall.glb",
                                std::make_shared<Material>("ggx.vert", "ggx.frag"))));
                        mc->castShadows = false;
                    };

                const float edgeYdown = (2 * cy - y_size) * 40.0f - 40.0f;
                const float edgeYup = (2 * cy + 2 - y_size) * 40.0f - 40.0f;
                const float edgeXleft = (2 * cx - x_size) * 40.0f - 40.0f;
                const float edgeXright = (2 * cx + 2 - x_size) * 40.0f - 40.0f;

                makeSpoke(CellCardinalDirection::Down,
                    glm::vec3(centerX, (centerY + edgeYdown) / 2.0f, 0.0f),
                    glm::vec3(90.0f, 0.0f, 0.0f));
                makeSpoke(CellCardinalDirection::Up,
                    glm::vec3(centerX, (centerY + edgeYup) / 2.0f, 0.0f),
                    glm::vec3(90.0f, 0.0f, 0.0f));
                makeSpoke(CellCardinalDirection::Left,
                    glm::vec3((centerX + edgeXleft) / 2.0f, centerY, 0.0f),
                    glm::vec3(0.0f, 90.0f, 0.0f));
                makeSpoke(CellCardinalDirection::Right,
                    glm::vec3((centerX + edgeXright) / 2.0f, centerY, 0.0f),
                    glm::vec3(0.0f, 90.0f, 0.0f));
            }

        // --- Render systems ---
        world.AddSystem(std::make_unique<CameraFollowSystem>());
        world.AddSystem(std::make_unique<OnDeathRenderSystem>());
        world.AddSystem(std::make_unique<ChargingBulletRenderSystem>());
        world.AddSystem(std::make_unique<LinkThrusterToShipSystem>());
        world.AddSystem(std::make_unique<LaserWallRenderSystem>());
		world.AddSystem(std::make_unique<UpdateListenerTransformSystem>());
        world.AddSystem(std::make_unique<DestroyTimerSystem>());

        //world.GetEntityManager().AddComponent<AudioListenerComponent>(camera, AudioListenerComponent{});
        AudioManager::PlayMusic("song.wav", true);
        AudioManager::SetMusicVolume(0.25f);
    }

    void Interpolate(
        const GameStateBlob& previousServerState,
        const GameStateBlob& currentServerState,
        const GameStateBlob& previousLocalState,
        const GameStateBlob& currentLocalState,
        GameStateBlob& renderState,
        float serverInterpolation,
        float localInterpolation) override
    {
        const AsteroidShooterGameState& prevServer = *reinterpret_cast<const AsteroidShooterGameState*>(previousServerState.data);
        const AsteroidShooterGameState& currServer = *reinterpret_cast<const AsteroidShooterGameState*>(currentServerState.data);
        const AsteroidShooterGameState& prevLocal = *reinterpret_cast<const AsteroidShooterGameState*>(previousLocalState.data);
        const AsteroidShooterGameState& currLocal = *reinterpret_cast<const AsteroidShooterGameState*>(currentLocalState.data);

        AsteroidShooterGameState& rend = *reinterpret_cast<AsteroidShooterGameState*>(renderState.data);

        for (int i = 0; i < NUM_PLAYERS; ++i) {
            if (playerId == i)
            {
                rend.posX[i] = prevLocal.posX[i] + (currLocal.posX[i] - prevLocal.posX[i]) * localInterpolation;
                rend.posY[i] = prevLocal.posY[i] + (currLocal.posY[i] - prevLocal.posY[i]) * localInterpolation;
                float delta = currLocal.rot[i] - prevLocal.rot[i];
                while (delta > 180.0f) delta -= 360.0f;
                while (delta < -180.0f) delta += 360.0f;
                rend.rot[i] = prevLocal.rot[i] + delta * localInterpolation;
            }
            else
            {
                rend.posX[i] = prevServer.posX[i] + (currServer.posX[i] - prevServer.posX[i]) * serverInterpolation;
                rend.posY[i] = prevServer.posY[i] + (currServer.posY[i] - prevServer.posY[i]) * serverInterpolation;
                float delta = currServer.rot[i] - prevServer.rot[i];
                while (delta > 180.0f) delta -= 360.0f;
                while (delta < -180.0f) delta += 360.0f;
                rend.rot[i] = prevServer.rot[i] + delta * serverInterpolation;
            }

            rend.health[i] = currServer.health[i];
            rend.alive[i] = currServer.alive[i];
            rend.remaingShootFrames[i] = currServer.remaingShootFrames[i];
            rend.isMovingForward[i] = currServer.isMovingForward[i];
            rend.shipInclination[i] = currServer.shipInclination[i];
            rend.isShooting[i] = currServer.isShooting[i];
            rend.shootCooldown[i] = currServer.shootCooldown[i];
            rend.velX[i] = currServer.velX[i];
            rend.velY[i] = currServer.velY[i];
            rend.angularVel[i] = currServer.angularVel[i];
        }

        // Bullets
        rend.bulletCount = currServer.bulletCount;
        for (int i = 0; i < MAX_BULLETS; ++i) {
            const Bullet& prevB = prevServer.bullets[i];
            const Bullet& currB = currServer.bullets[i];
            Bullet& rendB = rend.bullets[i];

            if (currB.active) {
                if (prevB.active) {
                    rendB.id = currB.id;
                    rendB.active = true;
                    rendB.posX = prevB.posX + (currB.posX - prevB.posX) * serverInterpolation;
                    rendB.posY = prevB.posY + (currB.posY - prevB.posY) * serverInterpolation;
                    rendB.velX = currB.velX;
                    rendB.velY = currB.velY;
                    rendB.ownerId = currB.ownerId;
                    rendB.lifetime = currB.lifetime;
                }
                else {
                    rendB = currB;
                }
            }
            else {
                rendB.active = false;
            }
        }

        // Walls and arena state — copy directly from server (no interpolation)
        std::memcpy(rend.hWalls, currServer.hWalls, sizeof(currServer.hWalls));
        std::memcpy(rend.vWalls, currServer.vWalls, sizeof(currServer.vWalls));
        std::memcpy(rend.cWalls, currServer.cWalls, sizeof(currServer.cWalls));
        std::memcpy(rend.tilesActive, currServer.tilesActive, sizeof(currServer.tilesActive));
        std::memcpy(rend.hWallsWarning, currServer.hWallsWarning, sizeof(currServer.hWallsWarning));
        std::memcpy(rend.vWallsWarning, currServer.vWallsWarning, sizeof(currServer.vWallsWarning));
        std::memcpy(rend.cWallsWarning, currServer.cWallsWarning, sizeof(currServer.cWallsWarning));
        std::memcpy(rend.tilesWarning, currServer.tilesWarning, sizeof(currServer.tilesWarning));
    }

    ~AsteroidShooterGameRenderer() override {
        //
    }

private:
    OpenGLWindow* window;
};

#endif