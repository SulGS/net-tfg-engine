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

#include "OpenAL/AudioManager.hpp"
#include "OpenAL/AudioComponents.hpp"


class AsteroidShooterGame : public IECSGameLogic {
private:
    int debugTicks = 0;
public:

    void printGameState(const AsteroidShooterGameState& state) const
    {
        Debug::Info("GameState") << "=== Asteroid Shooter Game State ===\n";

        // Players
        Debug::Info("GameState") << "Players:\n";
        for (int i = 0; i < 2; i++) {
            Debug::Info("GameState") << "  Player " << i << ":\n";
            Debug::Info("GameState") << "    Position: ("
                << state.posX[i] << ", "
                << state.posY[i] << ")\n";
            Debug::Info("GameState") << "    Rotation: "
                << state.rot[i] << " degrees\n";
            Debug::Info("GameState") << "    Health: "
                << state.health[i] << "\n";
            Debug::Info("GameState") << "    Alive: "
                << (state.alive[i] ? "true" : "false") << "\n";
            Debug::Info("GameState") << "    Remaining shoot frames: "
                << state.remaingShootFrames[i] << "\n";
            Debug::Info("GameState") << "    Shoot Cooldown: "
                << state.shootCooldown[i] << "\n";
            Debug::Info("GameState") << "    Death Cooldown: "
                << state.deathCooldown[i] << "\n";
        }

        // Bullets
        Debug::Info("GameState") << "Bullets:\n";
        Debug::Info("GameState") << "  Active Bullet Count: "
            << state.bulletCount << "\n";

        if (state.bulletCount > 0) {
            Debug::Info("GameState") << "  Active Bullets:\n";
            for (int i = 0; i < MAX_BULLETS; i++) {
                const Bullet& b = state.bullets[i];
                if (!b.active)
                    continue;

                Debug::Info("GameState") << "    Bullet #" << b.id << "\n";
                Debug::Info("GameState") << "      Position: ("
                    << b.posX << ", "
                    << b.posY << ")\n";
                Debug::Info("GameState") << "      Velocity: ("
                    << b.velX << ", "
                    << b.velY << ")\n";
                Debug::Info("GameState") << "      Owner: Player "
                    << b.ownerId << "\n";
                Debug::Info("GameState") << "      Lifetime: "
                    << b.lifetime << " frames\n";
            }
        }
        else {
            Debug::Info("GameState") << "  No active bullets\n";
        }

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
        if (Input::KeyPressed(Input::CharToKeycode('s'))) m |= INPUT_DOWN;
        if (Input::KeyPressed(Input::CharToKeycode(' '))) m |= INPUT_SHOOT;  // Space bar
        
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
            transform->setRotation(glm::vec3(0.0f, 0.0f, static_cast<float>(s.rot[p])));
            ship->shootCooldown = s.shootCooldown[p];
            ship->health = s.health[p];
            ship->remainingShootFrames = s.remaingShootFrames[p];
            ship->deathCooldown = s.deathCooldown[p];
			ship->isAlive = s.alive[p];
        }

        // First, collect all existing bullet IDs in ECS
        std::set<int> ecsActiveBulletIds;
        auto bulletQuery = world.GetEntityManager().CreateQuery<ECSBullet>();
        for (auto [entity, ecsb] : bulletQuery) {
            ecsActiveBulletIds.insert(ecsb->id);
        }

        // Create a set of active bullet IDs from state
        std::set<int> stateActiveBulletIds;
        for (int i = 0; i < MAX_BULLETS; i++) {
            if (s.bullets[i].active) {
                stateActiveBulletIds.insert(s.bullets[i].id);
            }
        }

        // Remove bullets that are in ECS but not in state
        auto bulletQuery2 = world.GetEntityManager().CreateQuery<ECSBullet>();
        for (auto [entity, ecsb] : bulletQuery2) {
            if (stateActiveBulletIds.find(ecsb->id) == stateActiveBulletIds.end()) {
                world.GetEntityManager().DestroyEntity(entity);
            }
        }

        // Update or create bullets from state
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
    }

    void ECSWorld_To_GameState(GameStateBlob& state) {
        AsteroidShooterGameState& s = *reinterpret_cast<AsteroidShooterGameState*>(state.data);
        
        // Clear state
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
            s.health[p] = ship->health;
            s.deathCooldown[p] = ship->deathCooldown;
			s.alive[p] = ship->isAlive;
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

		state.len = sizeof(AsteroidShooterGameState);

        //if (Input::KeyPressed(Input::CharToKeycode('p')))
        //{
		//	printGameState(s);
        //}
    }

    bool CompareStates(const GameStateBlob& a, const GameStateBlob& b) const override {
        return std::memcmp(a.data, b.data, sizeof(AsteroidShooterGameState)) == 0;
    }

    void InitECSLogic(GameStateBlob& state) override {
        AsteroidShooterGameState* s = reinterpret_cast<AsteroidShooterGameState*>(state.data);
        
        // Initialize players
        s->posX[0] = -10; s->posY[0] = -10;
        s->posX[1] =  10; s->posY[1] =  10;
        s->rot[0] = 0; s->rot[1] = 180;
        
        // Initialize bullets (all inactive)
        for (int i = 0; i < MAX_BULLETS; i++) {
            s->bullets[i].id = -1;
            s->bullets[i].active = false;
        }
        s->bulletCount = 0;

        s->remaingShootFrames[0] = -1;
        s->remaingShootFrames[1] = -1;
        
        // Initialize cooldowns
        s->shootCooldown[0] = 0;
        s->shootCooldown[1] = 0;

        s->health[0] = 100;
        s->health[1] = 100;

		s->alive[0] = true;
		s->alive[1] = true;

        s->deathCooldown[0] = 0;
        s->deathCooldown[1] = 0;

        state.len = sizeof(AsteroidShooterGameState);

        world.GetEntityManager().RegisterComponentType<SpaceShip>();
        world.GetEntityManager().RegisterComponentType<ECSBullet>();

        Entity player1 = world.GetEntityManager().CreateEntity();
        Transform* t1 = world.GetEntityManager().AddComponent<Transform>(player1, Transform{});
        t1->setPosition(glm::vec3(s->posX[0], s->posY[0], 0.0f));
        t1->setRotation(glm::vec3(0.0f, 0.0f, 0.0f));

        world.GetEntityManager().AddComponent<Playable>(player1, Playable{0, MakeZeroInputBlob(), (0 == playerId ? true : false)});
        world.GetEntityManager().AddComponent<SpaceShip>(player1, SpaceShip{100,-1,0,0,true});

        Entity player2 = world.GetEntityManager().CreateEntity();
        Transform* t2 = world.GetEntityManager().AddComponent<Transform>(player2, Transform{});
        t2->setPosition(glm::vec3(s->posX[1], s->posY[1], 0.0f));
        t2->setRotation(glm::vec3(0.0f, 0.0f, 180.0f));
        
        world.GetEntityManager().AddComponent<Playable>(player2, Playable{1, MakeZeroInputBlob(), (1 == playerId ? true : false)});
        world.GetEntityManager().AddComponent<SpaceShip>(player2, SpaceShip{100,-1,0,0,true});

        

        if (isServer) 
        {
            BoxCollider2D* collider1 = world.GetEntityManager().AddComponent<BoxCollider2D>(player1, BoxCollider2D{ glm::vec2(1.5f, 3.0f) });
            collider1->layer = CollisionLayer::PLAYER;
            collider1->collidesWith = CollisionLayer::BULLET;

            BoxCollider2D* collider2 = world.GetEntityManager().AddComponent<BoxCollider2D>(player2, BoxCollider2D{ glm::vec2(1.5f, 3.0f) });
            collider2->layer = CollisionLayer::PLAYER;
            collider2->collidesWith = CollisionLayer::BULLET;
        }
        
        
        world.AddSystem(std::make_unique<InputSystem>());
        if (isServer) 
        {
            world.AddSystem(std::make_unique<InputServerSystem>());
        }
        
        world.AddSystem(std::make_unique<BulletSystem>());
        world.AddSystem(std::make_unique<OnDeathLogicSystem>());

        eventProcessor->RegisterHandler(AsteroidEventMask::SPAWN_BULLET, std::make_unique<SpawnBulletHandler>());
        eventProcessor->RegisterHandler(AsteroidEventMask::BULLET_COLLIDES,std::make_unique<BulletCollidesHandler>());
		eventProcessor->RegisterHandler(AsteroidEventMask::DEATH, std::make_unique<DeathHandler>());
		eventProcessor->RegisterHandler(AsteroidEventMask::RESPAWN, std::make_unique<RespawnHandler>());

		deltaProcessor->RegisterHandler(DELTA_GAME_POSITIONS, std::make_unique<GamePositionsDeltaHandler>());
        
        
		
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

        // Players
        Debug::Info("GameState") << "Players:\n";
        for (int i = 0; i < 2; i++) {
            Debug::Info("GameState") << "  Player " << i << ":\n";
            Debug::Info("GameState") << "    Position: ("
                << state.posX[i] << ", "
                << state.posY[i] << ")\n";
            Debug::Info("GameState") << "    Rotation: "
                << state.rot[i] << " degrees\n";
            Debug::Info("GameState") << "    Health: "
                << state.health[i] << "\n";
            Debug::Info("GameState") << "    Alive: "
                << (state.alive[i] ? "true" : "false") << "\n";
            Debug::Info("GameState") << "    Remaining shoot frames: "
                << state.remaingShootFrames[i] << "\n";
            Debug::Info("GameState") << "    Shoot Cooldown: "
                << state.shootCooldown[i] << "\n";
            Debug::Info("GameState") << "    Death Cooldown: "
                << state.deathCooldown[i] << "\n";
        }

        // Bullets
        Debug::Info("GameState") << "Bullets:\n";
        Debug::Info("GameState") << "  Active Bullet Count: "
            << state.bulletCount << "\n";

        if (state.bulletCount > 0) {
            Debug::Info("GameState") << "  Active Bullets:\n";
            for (int i = 0; i < MAX_BULLETS; i++) {
                const Bullet& b = state.bullets[i];
                if (!b.active)
                    continue;

                Debug::Info("GameState") << "    Bullet #" << b.id << "\n";
                Debug::Info("GameState") << "      Position: ("
                    << b.posX << ", "
                    << b.posY << ")\n";
                Debug::Info("GameState") << "      Velocity: ("
                    << b.velX << ", "
                    << b.velY << ")\n";
                Debug::Info("GameState") << "      Owner: Player "
                    << b.ownerId << "\n";
                Debug::Info("GameState") << "      Lifetime: "
                    << b.lifetime << " frames\n";
            }
        }
        else {
            Debug::Info("GameState") << "  No active bullets\n";
        }

        Debug::Info("GameState") << "===================================\n";
    }
    
    void GameState_To_ECSWorld(const GameStateBlob& state)
    {
        AsteroidShooterGameState s =
            *reinterpret_cast<const AsteroidShooterGameState*>(state.data);

        EntityManager& em = world.GetEntityManager();

        // ============================================================
        // 1. SNAPSHOT CLEANUP (derived / visual-only state)
        // ============================================================

        // Remove all charging shoot effects (purely visual, snapshot-derived)
        {
            auto q = em.CreateQuery<ChargingShootEffect>();
            for (auto [e, effect] : q)
            {
                em.DestroyEntity(e);
            }
        }

        // Optional: reset UI state (prevents death/health UI layout conflicts)
        {
            auto q = em.CreateQuery<UIElement, UIText>();
            for (auto [e, element, text] : q)
            {
                text->text.clear();
                element->anchor = UIAnchor::TOP_LEFT;
                element->pivot = glm::vec2(0.0f);
                element->position = glm::vec2(0.0f);
            }
        }

        // ============================================================
        // 2. PLAYER STATE SYNC (authoritative)
        // ============================================================

        auto playerQuery =
            em.CreateQuery<Transform, Playable, SpaceShip, MeshComponent>();

        for (auto [entity, transform, play, ship, mesh] : playerQuery)
        {
            int p = play->playerId;

            transform->setPosition(
                glm::vec3(s.posX[p], s.posY[p], 0.0f)
            );

            transform->setRotation(
                glm::vec3(0.0f, 0.0f, s.rot[p] + 90.0f)
            );

            ship->shootCooldown = s.shootCooldown[p];
            ship->health = s.health[p];
            ship->remainingShootFrames = s.remaingShootFrames[p];

            // Clamp defensively at snapshot boundary
            ship->deathCooldown = std::max(0, s.deathCooldown[p]);

            // --- ALIVE EDGE DETECTION ---
            bool wasAlive = ship->isAlive;
            ship->isAlive = s.alive[p];

            if (!wasAlive && ship->isAlive)
            {
                // Respawn: restore visuals + reset derived state
                mesh->enabled = true;
                ship->remainingShootFrames = -1;
            }
            else if (wasAlive && !ship->isAlive)
            {
                // Death
                mesh->enabled = false;
            }
        }

        // ============================================================
        // 3. BULLET RECONCILIATION (authoritative)
        // ============================================================

        // Collect active bullet IDs from ECS
        std::set<int> ecsActiveBulletIds;
        {
            auto q = em.CreateQuery<ECSBullet>();
            for (auto [e, ecsb] : q)
            {
                ecsActiveBulletIds.insert(ecsb->id);
            }
        }

        // Collect active bullet IDs from snapshot
        std::set<int> stateActiveBulletIds;
        for (int i = 0; i < MAX_BULLETS; i++)
        {
            if (s.bullets[i].active)
            {
                stateActiveBulletIds.insert(s.bullets[i].id);
            }
        }

        // Remove ECS bullets not present in snapshot
        {
            auto q = em.CreateQuery<ECSBullet, MeshComponent>();
            for (auto [e, ecsb, mesh] : q)
            {
                if (stateActiveBulletIds.find(ecsb->id) ==
                    stateActiveBulletIds.end())
                {
                    em.DestroyEntity(e);
                }
            }
        }

        // Update or create bullets from snapshot
        for (int i = 0; i < MAX_BULLETS; i++)
        {
            const Bullet& b = s.bullets[i];
            if (!b.active)
                continue;

            bool found = false;

            auto q = em.CreateQuery<Transform, ECSBullet, MeshComponent>();
            for (auto [e, transform, ecsb, mesh] : q)
            {
                if (ecsb->id == b.id)
                {
                    transform->setPosition(
                        glm::vec3(b.posX, b.posY, 0.0f)
                    );

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
                // Create new bullet entity
                Entity newBullet = em.CreateEntity();

                Transform* t =
                    em.AddComponent<Transform>(newBullet, Transform{});

                t->setPosition(
                    glm::vec3(b.posX, b.posY, 0.0f)
                );

                auto bulletMat =
                    std::make_shared<Material>("generic.vert", "generic.frag");

                bulletMat->setVec3("uColor", glm::vec3(1.0f, 1.0f, 0.0f));

                em.AddComponent<ECSBullet>(
                    newBullet,
                    ECSBullet{ b.id, b.velX, b.velY, b.ownerId, b.lifetime }
                );

                Mesh* m = new Mesh("bullet.glb", bulletMat);
                em.AddComponent<MeshComponent>(newBullet, MeshComponent(m));

                // Spawn bullet sound (non-authoritative, time-limited)
                Entity bulletSound = em.CreateEntity();

                Transform* st =
                    em.AddComponent<Transform>(bulletSound, Transform{});

                st->setPosition(
                    glm::vec3(b.posX, b.posY, 0.0f)
                );

                DestroyTimer* dt =
                    em.AddComponent<DestroyTimer>(bulletSound, DestroyTimer{});

                dt->framesRemaining = RENDER_TICKS_PER_SECOND * 3;

                AudioSourceComponent* audio =
                    em.AddComponent<AudioSourceComponent>(
                        bulletSound,
                        AudioSourceComponent("shoot.wav",
                            AudioChannel::SFX,
                            false)
                    );

                audio->play = true;
            }
        }
    }


    void InitECSRenderer(const GameStateBlob& state, OpenGLWindow* window) override {
        this->window = window;  // Store window pointer for later use
        
        
        std::vector<unsigned int> triangleInds = {0, 1, 2};
        
        AsteroidShooterGameState s;
        std::memcpy(&s, state.data, sizeof(AsteroidShooterGameState));

        world.GetEntityManager().RegisterComponentType<SpaceShip>();
        world.GetEntityManager().RegisterComponentType<ECSBullet>();
        world.GetEntityManager().RegisterComponentType<ChargingShootEffect>();
		world.GetEntityManager().RegisterComponentType<DestroyTimer>();
        

        Entity player1 = world.GetEntityManager().CreateEntity();
        Transform* t1 = world.GetEntityManager().AddComponent<Transform>(player1, Transform{});
        t1->setPosition(glm::vec3(s.posX[0], s.posY[0], 0.0f));
        t1->setRotation(glm::vec3(0.0f, 0.0f, 90.0f));
        t1->setScale(glm::vec3(1.0f, 1.0f, 1.0f));

        auto player1Mat = std::make_shared<Material>("generic_texture.vert", "generic_texture.frag");
        //player1Mat->setVec3("uColor", glm::vec3(1.0f, 0.0f, 0.0f));
        
        world.GetEntityManager().AddComponent<Playable>(player1, Playable{0, MakeZeroInputBlob(), (0 == playerId ? true : false)});
        world.GetEntityManager().AddComponent<SpaceShip>(player1, SpaceShip{100,-1,0,0,true});
        world.GetEntityManager().AddComponent<MeshComponent>(player1, MeshComponent(new Mesh("ship.glb", player1Mat)));

        Entity player2 = world.GetEntityManager().CreateEntity();
        Transform* t2 = world.GetEntityManager().AddComponent<Transform>(player2, Transform{});
        t2->setPosition(glm::vec3(s.posX[1], s.posY[1], 0.0f));
        t2->setRotation(glm::vec3(0.0f, 0.0f, 270.0f));
        t2->setScale(glm::vec3(1.0f, 1.0f, 1.0f));

        auto player2Mat = std::make_shared<Material>("generic_texture.vert", "generic_texture.frag");
        //player2Mat->setVec3("uColor", glm::vec3(0.0f, 0.0f, 1.0f));
        
        world.GetEntityManager().AddComponent<Playable>(player2, Playable{1, MakeZeroInputBlob(), (1 == playerId ? true : false)});
        world.GetEntityManager().AddComponent<SpaceShip>(player2, SpaceShip{100,-1,0,0,true});
        world.GetEntityManager().AddComponent<MeshComponent>(player2, MeshComponent(new Mesh("ship.glb", player2Mat)));

        Entity camera = world.GetEntityManager().CreateEntity();
        Transform* camTrans = world.GetEntityManager().AddComponent<Transform>(camera, Transform{});
        // Place camera above the scene on Z axis so it looks down at origin
        camTrans->setPosition(glm::vec3(0.0f, 0.0f, 18.0f));
        Camera* camSettings = world.GetEntityManager().AddComponent<Camera>(camera, Camera{});
            
		camSettings->setPerspective(45.0f, window->getAspectRatio(), 0.001f, 1000.0f);
        camSettings->setTarget(glm::vec3(0.0f, 0.0f, 0.0f));
        camSettings->setUp(glm::vec3(0.0f, 1.0f, 0.0f));


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
        text->SetColor(1.0f, 1.0f, 1.0f, 1.0f);  // White
        text->SetFont("default");

        Entity escenario = world.GetEntityManager().CreateEntity();
        Transform* tesc = world.GetEntityManager().AddComponent<Transform>(escenario, Transform{});
        tesc->setPosition(glm::vec3(0.0f, 0.0f, -2.0f));
        tesc->setRotation(glm::vec3(90.0f, 0.0f, 0.0f));
        tesc->setScale(glm::vec3(6.0f, 6.0f, 6.0f));

        auto escenarioMat = std::make_shared<Material>("generic_texture.vert", "generic_texture.frag");
        //player1Mat->setVec3("uColor", glm::vec3(1.0f, 0.0f, 0.0f));
        world.GetEntityManager().AddComponent<MeshComponent>(escenario, MeshComponent(new Mesh("escenario.glb", escenarioMat)));

        // Add camera follow system so camera follows local player
        world.AddSystem(std::make_unique<CameraFollowSystem>());
        world.AddSystem(std::make_unique<OnDeathRenderSystem>());
        world.AddSystem(std::make_unique<ChargingBulletRenderSystem>());
		world.AddSystem(std::make_unique<DestroyTimerSystem>());


        AudioListenerComponent* listener = world.GetEntityManager().AddComponent<AudioListenerComponent>(camera, AudioListenerComponent{});

        AudioManager::SetEntityManager(&world.GetEntityManager());
        AudioManager::PlayMusic("song.wav", true);
        AudioManager::SetMusicVolume(0.25f);
    }

    void Interpolate(const GameStateBlob& previousServerState, const GameStateBlob& currentServerState, const GameStateBlob& previousLocalState, const GameStateBlob& currentLocalState, GameStateBlob& renderState, float serverInterpolation, float localInterpolation) override {
        // Deserializa los estados
        const AsteroidShooterGameState& prevServer = *reinterpret_cast<const AsteroidShooterGameState*>(previousServerState.data);
        const AsteroidShooterGameState& currServer = *reinterpret_cast<const AsteroidShooterGameState*>(currentServerState.data);
        const AsteroidShooterGameState& prevLocal = *reinterpret_cast<const AsteroidShooterGameState*>(previousLocalState.data);
        const AsteroidShooterGameState& currLocal = *reinterpret_cast<const AsteroidShooterGameState*>(currentLocalState.data);

        AsteroidShooterGameState& rend = *reinterpret_cast<AsteroidShooterGameState*>(renderState.data);

        // Interpola jugadores
        for (int i = 0; i < 2; ++i) {
            if (playerId == i)
            {
                rend.posX[i] = prevLocal.posX[i] + (currLocal.posX[i] - prevLocal.posX[i]) * localInterpolation;
                rend.posY[i] = prevLocal.posY[i] + (currLocal.posY[i] - prevLocal.posY[i]) * localInterpolation;
                float delta = currLocal.rot[i] - prevLocal.rot[i];
                while (delta > 180.0f) delta -= 360.0f;
                while (delta < -180.0f) delta += 360.0f;
                rend.rot[i] = prevLocal.rot[i] + delta * localInterpolation;
                rend.remaingShootFrames[i] = currLocal.remaingShootFrames[i];

                // USE LOCAL STATE FOR LOCAL PLAYER
                rend.alive[i] = currLocal.alive[i];
            }
            else
            {
                rend.posX[i] = prevServer.posX[i] + (currServer.posX[i] - prevServer.posX[i]) * serverInterpolation;
                rend.posY[i] = prevServer.posY[i] + (currServer.posY[i] - prevServer.posY[i]) * serverInterpolation;
                float delta = currServer.rot[i] - prevServer.rot[i];
                while (delta > 180.0f) delta -= 360.0f;
                while (delta < -180.0f) delta += 360.0f;
                rend.rot[i] = prevServer.rot[i] + delta * serverInterpolation;
                rend.remaingShootFrames[i] = currServer.remaingShootFrames[i];

                // USE SERVER STATE FOR REMOTE PLAYERS
                rend.alive[i] = currServer.alive[i];
            }

            // Health, cooldowns still use server (authority)
            rend.health[i] = currServer.health[i];
            rend.shootCooldown[i] = currServer.shootCooldown[i];
            rend.deathCooldown[i] = currServer.deathCooldown[i];
            // REMOVE rend.alive[i] = currServer.alive[i]; from here
        }

        // Interpola balas
        rend.bulletCount = currServer.bulletCount;
        for (int i = 0; i < MAX_BULLETS; ++i) {
            const Bullet& prevServerBullet = prevServer.bullets[i];
            const Bullet& currServerBullet = currServer.bullets[i];
			const Bullet& prevLocalBullet = prevLocal.bullets[i];
			const Bullet& currLocalBullet = currLocal.bullets[i];

            Bullet& rendBullet = rend.bullets[i];

            if (currServerBullet.active) 
            {
                if (prevServerBullet.active)
                {
                    rendBullet.id = currServerBullet.id;
                    rendBullet.active = true;
                    rendBullet.posX = prevServerBullet.posX + (currServerBullet.posX - prevServerBullet.posX) * serverInterpolation;
                    rendBullet.posY = prevServerBullet.posY + (currServerBullet.posY - prevServerBullet.posY) * serverInterpolation;
                    rendBullet.velX = prevServerBullet.velX + (currServerBullet.velX - prevServerBullet.velX) * serverInterpolation;
                    rendBullet.velY = prevServerBullet.velY + (currServerBullet.velY - prevServerBullet.velY) * serverInterpolation;
                    rendBullet.ownerId = currServerBullet.ownerId;
                    rendBullet.lifetime = currServerBullet.lifetime;
                }
                else
                {
                    // Si solo está activa en el estado actual del servidor, copia el actual del servidor
                    rendBullet = currServerBullet;
                }
			}
			else
			{
				// Bala inactiva
				rendBullet.active = false;
            }
        }
    }

    ~AsteroidShooterGameRenderer() override {
        //
    }

private:
    OpenGLWindow* window;  // Store window pointer for cleanup
};

#endif