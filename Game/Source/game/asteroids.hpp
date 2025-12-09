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

    void printGameState(const AsteroidShooterGameState& state) const {
        printf("=== Asteroid Shooter Game State ===\n\n");

        // Print player positions and rotations
        printf("Players:\n");
        for (int i = 0; i < 2; i++) {
            printf("  Player %d:\n", i);
            printf("    Position: (%.2f, %.2f)\n", state.posX[i], state.posY[i]);
            printf("    Rotation: %f degrees\n", state.rot[i]);
            printf("    Health: %d\n", state.health[i]);
            printf("    Remaining shoot frames: %d\n", state.remaingShootFrames[i]);
            printf("    Shoot Cooldown: %d\n", state.shootCooldown[i]);
            printf("    Death Cooldown: %d\n", state.deathCooldown[i]);
        }

        // Print bullet information
        printf("\nBullets:\n");
        printf("  Active Bullet Count: %d\n", state.bulletCount);

        if (state.bulletCount > 0) {
            printf("  Active Bullets:\n");
            for (int i = 0; i < MAX_BULLETS; i++) {
                if (state.bullets[i].active) {
                    printf("    Bullet #%d:\n", state.bullets[i].id);
                    printf("      Position: (%.2f, %.2f)\n",
                        state.bullets[i].posX, state.bullets[i].posY);
                    printf("      Velocity: (%.2f, %.2f)\n",
                        state.bullets[i].velX, state.bullets[i].velY);
                    printf("      Owner: Player %d\n", state.bullets[i].ownerId);
                    printf("      Lifetime: %d frames\n", state.bullets[i].lifetime);
                }
            }
        }
        else {
            printf("  No active bullets\n");
        }

        printf("\n===================================\n");
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
    }

    bool CompareStates(const GameStateBlob& a, const GameStateBlob& b) const override {
        return std::memcmp(a.data, b.data, sizeof(AsteroidShooterGameState)) == 0;
    }

    void InitECSLogic(GameStateBlob& state) override {
        AsteroidShooterGameState* s = reinterpret_cast<AsteroidShooterGameState*>(state.data);
        
        // Initialize players
        s->posX[0] = -50; s->posY[0] = -50;
        s->posX[1] =  50; s->posY[1] =  50;
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
        else 
        {
            if (playerId == 0)
            {
                AudioListenerComponent* listener = world.GetEntityManager().AddComponent<AudioListenerComponent>(player1, AudioListenerComponent{});
            }
            else
            {
                AudioListenerComponent* listener = world.GetEntityManager().AddComponent<AudioListenerComponent>(player2, AudioListenerComponent{});
            }
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
        
        if (!isServer)
        {
            AudioManager::SetEntityManager(&world.GetEntityManager());
            AudioManager::PlayMusic("song.wav", true);
			AudioManager::SetMusicVolume(0.25f);
        }
		
    }

    void PrintState(const GameStateBlob& state) const override {
        AsteroidShooterGameState s;
        std::memcpy(&s, state.data, sizeof(AsteroidShooterGameState));
        printGameState(s);
    }
};




class AsteroidShooterGameRenderer : public IECSGameRenderer {
public:


std::vector<float> TriangleVerts() {
    // Base triangle centered at origin, pointing RIGHT (positive X)
    // The Transform component will handle position and rotation!
    std::vector<float> verts = {
        3.0f, 0.0f, 0.0f,      // Right point (nose) - points along +X axis
        -3.0f, -2.5f, 0.0f,    // Bottom left
        -3.0f, 2.5f, 0.0f      // Top left
    };
    
    return verts;
}
    

std::vector<float> BulletVerts() {
    // Centered bullet (square) at origin
    float size = 1.0f;
    
    return {
        -size, -size, 0.0f,  // Bottom-left
        size, -size, 0.0f,   // Bottom-right
        size, size, 0.0f,    // Top-right
        -size, size, 0.0f    // Top-left
    };
}



    void GameState_To_ECSWorld(const GameStateBlob& state) {
        AsteroidShooterGameState s = *reinterpret_cast<const AsteroidShooterGameState*>(state.data);

        auto query = world.GetEntityManager().CreateQuery<Transform, Playable, SpaceShip,MeshComponent>();
        for (auto [entity, transform, play, ship, mesh] : query) {
            int p = play->playerId;
            transform->setPosition(glm::vec3(s.posX[p], s.posY[p], 0.0f));
            transform->setRotation(glm::vec3(0.0f, 0.0f, s.rot[p]));

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
        auto bulletQuery2 = world.GetEntityManager().CreateQuery<ECSBullet,MeshComponent>();
        for (auto [entity, ecsb, mesh] : bulletQuery2) {
            if (stateActiveBulletIds.find(ecsb->id) == stateActiveBulletIds.end()) {
                //window->removeMesh(mesh->mesh);
                world.GetEntityManager().DestroyEntity(entity);
            }
        }

        float yellow[3] = {1.0f, 1.0f, 0.0f};
        std::vector<unsigned int> bulletInds = {0,1,2,2,3,0};

        // Update or create bullets from state
        for (int i = 0; i < MAX_BULLETS; i++) {
            const Bullet& b = s.bullets[i];
            if (b.active) {
                bool found = false;
                auto bulletQuery3 = world.GetEntityManager().CreateQuery<Transform, ECSBullet, MeshComponent>();
                for (auto [entity, transform, ecsb, mesh] : bulletQuery3) {
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
                    Transform* t = world.GetEntityManager().AddComponent<Transform>(newBullet, Transform{});
                    t->setPosition(glm::vec3(b.posX, b.posY, 0.0f));
                    
                    world.GetEntityManager().AddComponent<ECSBullet>(newBullet, ECSBullet{b.id, b.velX, b.velY, b.ownerId, b.lifetime});
                    Mesh* m = new Mesh(BulletVerts(), bulletInds, yellow);
                    world.GetEntityManager().AddComponent<MeshComponent>(newBullet, MeshComponent(m));
                }
            }
        }
    }

    

    void InitECSRenderer(const GameStateBlob& state, OpenGLWindow* window) override {
        this->window = window;  // Store window pointer for later use
        
        float red[3] = {1.0f, 0.0f, 0.0f};
        float blue[3] = {0.0f, 0.0f, 1.0f};
        
        std::vector<unsigned int> triangleInds = {0, 1, 2};
        
        AsteroidShooterGameState s;
        std::memcpy(&s, state.data, sizeof(AsteroidShooterGameState));

        world.GetEntityManager().RegisterComponentType<SpaceShip>();
        world.GetEntityManager().RegisterComponentType<ECSBullet>();
        world.GetEntityManager().RegisterComponentType<ChargingShootEffect>();
        

        Entity player1 = world.GetEntityManager().CreateEntity();
        Transform* t1 = world.GetEntityManager().AddComponent<Transform>(player1, Transform{});
        t1->setPosition(glm::vec3(s.posX[0], s.posY[0], 0.0f));
        t1->setRotation(glm::vec3(0.0f, 0.0f, 0.0f));
        
        world.GetEntityManager().AddComponent<Playable>(player1, Playable{0, MakeZeroInputBlob(), (0 == playerId ? true : false)});
        world.GetEntityManager().AddComponent<SpaceShip>(player1, SpaceShip{100,-1,0,0,true});
        world.GetEntityManager().AddComponent<MeshComponent>(player1, MeshComponent(new Mesh(TriangleVerts(), triangleInds, red)));

        Entity player2 = world.GetEntityManager().CreateEntity();
        Transform* t2 = world.GetEntityManager().AddComponent<Transform>(player2, Transform{});
        t2->setPosition(glm::vec3(s.posX[1], s.posY[1], 0.0f));
        t2->setRotation(glm::vec3(0.0f, 0.0f, 180.0f));
        
        world.GetEntityManager().AddComponent<Playable>(player2, Playable{1, MakeZeroInputBlob(), (1 == playerId ? true : false)});
        world.GetEntityManager().AddComponent<SpaceShip>(player2, SpaceShip{100,-1,0,0,true});
        world.GetEntityManager().AddComponent<MeshComponent>(player2, MeshComponent(new Mesh(TriangleVerts(), triangleInds, blue)));

        Entity camera = world.GetEntityManager().CreateEntity();
        Transform* camTrans = world.GetEntityManager().AddComponent<Transform>(camera, Transform{});
        // Place camera above the scene on Z axis so it looks down at origin
        camTrans->setPosition(glm::vec3(0.0f, 0.0f, 2.0f));
        Camera* camSettings = world.GetEntityManager().AddComponent<Camera>(camera, Camera{});
            
        camSettings->setOrthographic(-100.0f, 100.0f, -100.0f, 100.0f, 0.1f, 100.0f);
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

        // Add camera follow system so camera follows local player
        world.AddSystem(std::make_unique<CameraFollowSystem>());
        world.AddSystem(std::make_unique<OnDeathRenderSystem>());
        world.AddSystem(std::make_unique<ChargingBulletRenderSystem>());
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
                // Rotación: interpola linealmente (puedes mejorar con shortest path si lo necesitas)
                float delta = currLocal.rot[i] - prevLocal.rot[i];

                // Normaliza el delta al rango [-180, 180] para tomar el camino más corto
                while (delta > 180.0f) delta -= 360.0f;
                while (delta < -180.0f) delta += 360.0f;

                rend.rot[i] = prevLocal.rot[i] + delta * localInterpolation;

                rend.remaingShootFrames[i] = currLocal.remaingShootFrames[i];
            }
            else 
            {
                rend.posX[i] = prevServer.posX[i] + (currServer.posX[i] - prevServer.posX[i]) * serverInterpolation;
                rend.posY[i] = prevServer.posY[i] + (currServer.posY[i] - prevServer.posY[i]) * serverInterpolation;
                // Rotación: interpola linealmente, corrigiendo el camino más corto
                float delta = currServer.rot[i] - prevServer.rot[i];

                // Normaliza el delta al rango [-180, 180] para tomar el camino más corto
                while (delta > 180.0f) delta -= 360.0f;
                while (delta < -180.0f) delta += 360.0f;

                rend.rot[i] = prevServer.rot[i] + delta * serverInterpolation;
                rend.remaingShootFrames[i] = currServer.remaingShootFrames[i];
            }
            
            // No interpolamos salud ni cooldowns, solo copiamos el actual
            rend.health[i] = currServer.health[i];
            
            rend.shootCooldown[i] = currServer.shootCooldown[i];
            rend.deathCooldown[i] = currServer.deathCooldown[i];
			rend.alive[i] = currServer.alive[i];
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