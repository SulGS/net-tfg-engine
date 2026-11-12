#ifndef ASTEROIDS_LOGIC
#define ASTEROIDS_LOGIC

// Game simulation shared by GameServer and GameClient (prediction). Headless: nothing here may pull OpenGL, GLFW, OpenAL
// or the window/input layer, so GameServer builds and links against NetTFGEngineCore only. Client-only additions (local
// input, the exit-to-lobby check) live in AsteroidShooterClientGame (asteroids.hpp).

#include <glm/glm.hpp>
#include "netcode/netcode_common.hpp"
#include <memory>
#include <cstring>
#include <math.h>
#include <cmath>
#include "ecs/ecs.hpp"
#include "ecs/ecs_gamelogic.hpp"
#include "ecs/ecs_common.hpp"
#include "ecs/Collisions/BoxCollider2D.hpp"
#include "Utils/Debug/Debug.hpp"

#include "GameState.hpp"
#include "Components.hpp"
#include "ArenaLayout.hpp"
#include "LogicSystems/InputMask.hpp"
#include "LogicSystems/MatchStartSystem.hpp"
#include "LogicSystems/InputSystem.hpp"
#include "LogicSystems/InputServerSystem.hpp"
#include "LogicSystems/ArenaSystem.hpp"
#include "LogicSystems/GameOverSystem.hpp"
#include "LogicSystems/BulletSystem.hpp"
#include "LogicSystems/OnDeathLogicSystem.hpp"
#include "Events.hpp"
#include "EventHandlers.hpp"
#include "Deltas.hpp"
#include "DeltaHandler.hpp"
#include <random>
#include <set>
#include <unordered_set>

#include <openssl/evp.h>


// Pre-match freeze: see MatchStartTimer/MatchStartSystem in LogicSystems/MatchStartSystem.hpp.
inline constexpr int MATCH_START_COUNTDOWN_TICKS = 5 * TICKS_PER_SECOND;   // starts once every player has loaded (see MatchStartTimer)

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
    static const SpawnPoint table[NUM_PLAYERS] = {
        { 0, 0,  45.0f },   // bottom-left corner        → face NE
        { 3, 0,  83.0f },   // bottom edge, 1/3          → face N-NE
        {6, 0,  97.0f},   // bottom edge, 2/3          → face N-NW
        { 9, 0, 135.0f },   // bottom-right corner       → face NW
        { 9, 3, 153.0f },   // right edge, 1/3           → face W-NW
        { 9, 6, 167.0f },   // right edge, 2/3           → face W-SW
        { 9, 9, 225.0f },   // top-right corner          → face SW
        { 6, 9, 263.0f },   // top edge, 2/3             → face S-SW
        { 3, 9, 277.0f },   // top edge, 1/3             → face S-SE
        { 0, 9, 315.0f },   // top-left corner           → face SE
        { 0, 6, 333.0f },   // left edge, 2/3            → face E-SE  (going down)
        { 0, 3, 347.0f },   // left edge, 1/3            → face E-NE  (going down)
    };

    if (playerIndex < 0 || playerIndex >= NUM_PLAYERS)
        return table[0];
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
    // Set by OnAllPlayersReady (server tick thread, between ticks), consumed by the next SimulateFrame.
    std::atomic<bool> allPlayersReady_{ false };
public:

    // ArenaSystem stamps each wall's turn-on frame into its warning code (see EncodeWallWarning), so it needs the frame
    // being simulated; the ECS systems themselves only get deltaTime.
    void SimulateFrame(GameStateBlob& state, std::vector<EventEntry> events, std::map<int, InputEntry> inputs) override {
        if (isServer) {
            if (ArenaSystem* arena = world.GetSystem<ArenaSystem>())
                arena->SetCurrentFrame(state.frame);

            // Released into the world (and from there the state and its delta) on the next simulated frame.
            if (allPlayersReady_.exchange(false)) {
                auto timerQuery = world.GetEntityManager().CreateQuery<MatchStartTimer>();
                for (auto [entity, timer] : timerQuery)
                    timer->waitingForPlayers = false;
                Debug::Info("AsteroidShooterGame") << "All players ready at frame " << state.frame << ", start countdown running\n";
            }
        }
        IECSGameLogic::SimulateFrame(state, std::move(events), std::move(inputs));
    }

    void OnAllPlayersReady() override {
        allPlayersReady_.store(true);
    }

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

    // Only clients produce local input (AsteroidShooterClientGame reads InputMap); the server never calls this.
    InputBlob GenerateLocalInput() override {
        return MakeZeroInputBlob();
    }

    void GameState_To_ECSWorld(const GameStateBlob& state) override {
        AsteroidShooterGameState s = *reinterpret_cast<const AsteroidShooterGameState*>(state.data);

        auto query = world.GetEntityManager().CreateQuery<Transform, Playable, SpaceShip>();
        for (auto [entity, transform, play, ship] : query) {
            int p = play->playerId;
            transform->setPosition(glm::vec3(s.posX[p], s.posY[p], 0.0f));
            transform->setRotation(glm::vec3(0.0f, 0.0f, s.rot[p]));
            ship->shipZRotation = static_cast<int>(s.rot[p]);
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
            ship->dashTicks = s.dashTicks[p];
            ship->dashCooldown = s.dashCooldown[p];
            ship->shieldTicks = s.shieldTicks[p];
            ship->shieldCooldown = s.shieldCooldown[p];
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

        // Sync wall enabled/warning from the blob into logic ECS. ArenaSystem never runs client-side, so without this the
        // LaserWallIDs stay at their InitECSLogic defaults and prediction's ECSWorld_To_GameState round-trip writes those stale
        // values back into the snapshot chain, corrupting each reconciliation (the sparse delta format no longer masks it).
        {
            auto wallQuery = world.GetEntityManager().CreateQuery<LaserWallID>();
            for (auto [entity, lwid] : wallQuery)
            {
                if (world.GetEntityManager().GetComponent<CenterSpoke>(entity) != nullptr) continue;

                lwid->enabled = EdgeWallSlot(s.hWalls, s.vWalls, lwid->cellId, lwid->dir, lwid->half);
                lwid->warnCode = EdgeWallSlot(s.hWallsWarning, s.vWallsWarning, lwid->cellId, lwid->dir, lwid->half);
                lwid->warning = lwid->warnCode != 0;
            }

            auto spokeQuery = world.GetEntityManager().CreateQuery<LaserWallID, CenterSpoke>();
            for (auto [entity, lwid, spoke] : spokeQuery)
            {
                int cx = lwid->cellId / y_size;
                int cy = lwid->cellId % y_size;
                int dirIdx = SpokeIndex(lwid->dir);
                lwid->enabled = s.cWalls[cx][cy][dirIdx];
                lwid->warnCode = s.cWallsWarning[cx][cy][dirIdx];
                lwid->warning = lwid->warnCode != 0;
            }
        }

        // Match-start countdown
        {
            auto timerQuery = world.GetEntityManager().CreateQuery<MatchStartTimer>();
            for (auto [entity, timer] : timerQuery) {
                timer->ticksRemaining = s.startCountdownTicks;
                timer->waitingForPlayers = s.startWaitingForPlayers != 0;
            }
        }
    }

    void ECSWorld_To_GameState(GameStateBlob& state) override {
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
            s.dashTicks[p] = ship->dashTicks;
            s.dashCooldown[p] = ship->dashCooldown;
            s.shieldTicks[p] = ship->shieldTicks;
            s.shieldCooldown[p] = ship->shieldCooldown;
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

                EdgeWallSlot(s.hWalls, s.vWalls, lwid->cellId, lwid->dir, lwid->half) = lwid->enabled;
                EdgeWallSlot(s.hWallsWarning, s.vWallsWarning, lwid->cellId, lwid->dir, lwid->half) = lwid->warnCode;
            }
        }

        // Center spokes — enabled and warning
        {
            auto spokeQuery = world.GetEntityManager().CreateQuery<LaserWallID, CenterSpoke>();
            for (auto [entity, lwid, spoke] : spokeQuery)
            {
                int cx = lwid->cellId / y_size;
                int cy = lwid->cellId % y_size;

                int dirIdx = SpokeIndex(lwid->dir);
                s.cWalls[cx][cy][dirIdx] = lwid->enabled;
                s.cWallsWarning[cx][cy][dirIdx] = lwid->warnCode;
            }
        }

        // Match-start countdown
        {
            auto timerQuery = world.GetEntityManager().CreateQuery<MatchStartTimer>();
            for (auto [entity, timer] : timerQuery) {
                s.startCountdownTicks = timer->ticksRemaining;
                s.startWaitingForPlayers = timer->waitingForPlayers ? 1 : 0;
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

            s->dashTicks[i] = 0;
            s->dashCooldown[i] = 0;
            s->shieldTicks[i] = 0;
            s->shieldCooldown[i] = 0;
        }

        for (int i = 0; i < MAX_BULLETS; i++) {
            s->bullets[i].id = -1;
            s->bullets[i].active = false;
        }
        s->bulletCount = 0;

        s->startCountdownTicks = MATCH_START_COUNTDOWN_TICKS;
        s->startWaitingForPlayers = 1;

        state.len = sizeof(AsteroidShooterGameState);

        world.GetEntityManager().RegisterComponentType<SpaceShip>();
        world.GetEntityManager().RegisterComponentType<ECSBullet>();
        world.GetEntityManager().RegisterComponentType<TileID>();
        world.GetEntityManager().RegisterComponentType<PillarID>();
        world.GetEntityManager().RegisterComponentType<CenterSpoke>();
        world.GetEntityManager().RegisterComponentType<LaserWallID>();
        world.GetEntityManager().RegisterComponentType<SpectatorState>();
		world.GetEntityManager().RegisterComponentType<ExitButtonChecker>();
		world.GetEntityManager().RegisterComponentType<MatchStartTimer>();

		Entity exitCheckerEntity = world.GetEntityManager().CreateEntity();
        world.GetEntityManager().AddComponent<ExitButtonChecker>(exitCheckerEntity, ExitButtonChecker{});

		Entity matchStartEntity = world.GetEntityManager().CreateEntity();
        world.GetEntityManager().AddComponent<MatchStartTimer>(matchStartEntity, MatchStartTimer{ MATCH_START_COUNTDOWN_TICKS, true });


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
                // Half-extents matched to ship_low.glb (bounds X:[-0.359,0.357] Y:[-0.5,0.5], x5 in the renderer -> ~1.8 long,
                // ~2.5 wingspan; matches the thruster mount at x=-1.8). The old (1.5, 3.0) was ~17% short and ~20% wide,
                // causing missed hits at nose/tail and deaths to walls clear of the wingtips.
                BoxCollider2D* collider = world.GetEntityManager().AddComponent<BoxCollider2D>(player, BoxCollider2D{ glm::vec2(1.8f, 2.5f) });
                collider->layer = CollisionLayer::PLAYER;
                collider->collidesWith = CollisionLayer::BULLET | CollisionLayer::WALL;
            }
        }


        auto& em = world.GetEntityManager();

        // Arena geometry comes from ArenaLayout, shared with InitECSRenderer so both sides match one to one.
        const ArenaLayout::Layout arena = ArenaLayout::Build();

        // Tiles
        for (const auto& tile : arena.tiles)
        {
            Entity e = em.CreateEntity();
            Transform* t = em.AddComponent<Transform>(e, Transform{});
            t->setPosition(tile.pos);
            t->setScale(glm::vec3(1.0f));
            em.AddComponent<TileID>(e, TileID{ tile.cellId });
        }

        // Pillars (logic side — no mesh)
        for (const auto& pillar : arena.pillars)
        {
            Entity e = em.CreateEntity();
            Transform* t = em.AddComponent<Transform>(e, Transform{});
            t->setPosition(pillar.pos);
            t->setRotation(glm::vec3(90.0f, 0.0f, 0.0f));
            t->setScale(glm::vec3(1.0f));
            em.AddComponent<PillarID>(e, pillar.id);
        }

        std::mt19937 initRng{ std::random_device{}() };
        std::uniform_real_distribution<float> initDist(3.0f, 30.0f);

        // Walls and spokes kill a player on contact (server only: clients have no colliders).
        auto onWallHit = [this](Entity self, Entity other, const CollisionInfo& info) {
            Playable* play = this->world.GetEntityManager().GetComponent<Playable>(other);
            if (!play) return; // only react to players

            EventEntry deathEvent;
            deathEvent.event.type = AsteroidEventMask::DEATH;
            DeathEventData deathData;
            deathData.playerId = play->playerId;
            std::memcpy(deathEvent.event.data, &deathData, sizeof(DeathEventData));
            deathEvent.event.len = sizeof(DeathEventData);
            this->world.GetEvents().push_back(deathEvent);
            };

        // Walls: one logic entity per half cell side (see ArenaLayout); each half has its own state slot (EdgeWallSlot),
        // so what collides on the server is exactly what is serialized and drawn.
        for (const auto& w : arena.walls)
        {
            Entity e = em.CreateEntity();
            Transform* t = em.AddComponent<Transform>(e, Transform{});
            t->setPosition(w.pos);
            t->setRotation(w.rot);
            t->setScale(glm::vec3(2.0f, 2.0f, 19.0f));

            LaserWallID lwid(ArenaLayout::CellId(w.cellX, w.cellY), w.dir, w.half);
            lwid.enabled = w.onBorder;
            lwid.timer = initDist(initRng);
            em.AddComponent<LaserWallID>(e, lwid);

            if (isServer)
            {
                // Border/shared walls run along the shared edge between two cells:
                // Down/Up walls are wide along X, Left/Right walls are wide along Y.
                const bool wideX = (w.dir == CellCardinalDirection::Down || w.dir == CellCardinalDirection::Up);
                glm::vec2 halfExtents = wideX ? glm::vec2(19.0f, 2.0f) : glm::vec2(2.0f, 19.0f);

                BoxCollider2D* collider = em.AddComponent<BoxCollider2D>(e, BoxCollider2D{ halfExtents });
                collider->isEnabled = w.onBorder;
                collider->layer = CollisionLayer::WALL;
                collider->collidesWith = CollisionLayer::PLAYER | CollisionLayer::BULLET;
                collider->SetOnCollisionEnter(onWallHit);
            }
        }

        // Center spokes
        for (const auto& s : arena.spokes)
        {
            Entity e = em.CreateEntity();
            Transform* t = em.AddComponent<Transform>(e, Transform{});
            t->setPosition(s.pos);
            t->setRotation(s.rot);
            t->setScale(glm::vec3(2.0f, 2.0f, 19.0f));

            LaserWallID lwid(s.cellId, s.dir);
            lwid.enabled = false;
            lwid.timer = initDist(initRng);
            em.AddComponent<LaserWallID>(e, lwid);
            em.AddComponent<CenterSpoke>(e, CenterSpoke{});

            if (isServer)
            {
                // Spokes run from the cell centre to an edge midpoint, so their
                // orientation is the opposite of a border wall with the same dir:
                // Down/Up spokes are wide along Y, Left/Right spokes are wide along X.
                const bool wideX = (s.dir == CellCardinalDirection::Left || s.dir == CellCardinalDirection::Right);
                glm::vec2 halfExtents = wideX ? glm::vec2(19.0f, 2.0f) : glm::vec2(2.0f, 19.0f);

                BoxCollider2D* collider = em.AddComponent<BoxCollider2D>(e, BoxCollider2D{ halfExtents });
                collider->isEnabled = false;
                collider->layer = CollisionLayer::WALL;
                collider->collidesWith = CollisionLayer::PLAYER | CollisionLayer::BULLET;
                collider->SetOnCollisionEnter(onWallHit);
            }
        }


        // Must run before InputSystem/ArenaSystem so both see this tick's already-decremented value, not last tick's.
        world.AddSystem(std::make_unique<MatchStartSystem>());
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
        eventProcessor->RegisterHandler(AsteroidEventMask::WARN_TILE, std::make_unique<WarnTileHandler>());
        eventProcessor->RegisterHandler(AsteroidEventMask::BULLET_HIT_WALL, std::make_unique<BulletHitWallHandler>());

        deltaProcessor->RegisterHandler(DELTA_GAME_POSITIONS, std::make_unique<GamePositionsDeltaHandler>());
        deltaProcessor->RegisterHandler(DELTA_WALL_STATE, std::make_unique<WallStateDeltaHandler>());
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

        // Only what the client reconstructs exactly (events + deltas), so a mismatch means a real desync and the server
        // answers with a full state. Walls/tiles included: otherwise an arena desync was only healed by the periodic
        // wall keyframe (up to 3 s). Positions/bullets are left out: prediction legitimately differs there.
        EVP_DigestUpdate(ctx, s.health, sizeof(s.health));
        EVP_DigestUpdate(ctx, s.alive, sizeof(s.alive));
        EVP_DigestUpdate(ctx, s.tilesActive, sizeof(s.tilesActive));
        EVP_DigestUpdate(ctx, s.tilesWarning, sizeof(s.tilesWarning));
        EVP_DigestUpdate(ctx, s.hWalls, sizeof(s.hWalls));
        EVP_DigestUpdate(ctx, s.vWalls, sizeof(s.vWalls));
        EVP_DigestUpdate(ctx, s.cWalls, sizeof(s.cWalls));
        EVP_DigestUpdate(ctx, s.hWallsWarning, sizeof(s.hWallsWarning));
        EVP_DigestUpdate(ctx, s.vWallsWarning, sizeof(s.vWallsWarning));
        EVP_DigestUpdate(ctx, s.cWallsWarning, sizeof(s.cWallsWarning));

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

#endif
