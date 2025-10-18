#ifndef ASTEROIDS
#define ASTEROIDS

#include "OpenGL/OpenGLIncludes.hpp"
#include "netcode/netcode_common.hpp"
#include <memory>
#include <cstring>
#include "Utils/Input.hpp"
#include "OpenGL/IGameRenderer.hpp"
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

// Constants
const int MAX_BULLETS = 32;  // Adjust based on your needs

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
    float posX[2];
    float posY[2];
    int health[2];
    int rot[2];
    
    // Bullet pool
    Bullet bullets[MAX_BULLETS];
    int bulletCount;  // Number of active bullets (for quick iteration)
    
    // Shooting cooldown per player
    int shootCooldown[2];
    int deathCooldown[2];
};

class SpaceShip : public IComponent {
public:
    int health;
    int shootCooldown; // frames until can shoot again
    int deathCooldown;

    SpaceShip() : health(100),shootCooldown(0), deathCooldown(0) {}
    SpaceShip(int h,int cd, int dc) : health(h), shootCooldown(cd), deathCooldown(dc) {}
};

class ECSBullet : public IComponent {
public:
    int id;
    float velX;
    float velY;
    int ownerId;    // Which player shot it
    int lifetime;   // Frames remaining (for cleanup)

    ECSBullet() : id(-1), velX(0), velY(0), ownerId(-1), lifetime(0) {}
    ECSBullet(int i, float vx, float vy, int oid, int lt) : id(i), velX(vx), velY(vy), ownerId(oid), lifetime(lt) {}
};

enum InputMask : uint8_t {
    INPUT_NONE  = 0,
    INPUT_LEFT  = 1 << 0,
    INPUT_RIGHT = 1 << 1,
    INPUT_TOP   = 1 << 2,
    INPUT_DOWN  = 1 << 3,
    INPUT_SHOOT = 1 << 4  // New: Space to shoot
};

class BulletSystem : public ISystem {
public:
    void Update(EntityManager& entityManager, float deltaTime) override {
        const float WORLD_SIZE = 400.0f;

        //std::cout << "BulletSystem Update\n";
        
        auto query = entityManager.CreateQuery<Transform, ECSBullet>();
        for (auto [entity, transform, ecsb] : query) {
            //std::cout << "  Bullet ID: " << ecsb->id << " Pos: (" << pos->x << ", " << pos->y << ") Vel: (" << ecsb->velX << ", " << ecsb->velY << ") Lifetime: " << ecsb->lifetime << "\n";
            // Move bullet

            transform->translate(glm::vec3(ecsb->velX, ecsb->velY, 0.0f));
            
            // Decrease lifetime
            ecsb->lifetime--;
            
            // Deactivate if lifetime expired or out of bounds
            if (ecsb->lifetime <= 0 ||
                transform->getPosition().x < -WORLD_SIZE || transform->getPosition().x > WORLD_SIZE ||
                transform->getPosition().y < -WORLD_SIZE || transform->getPosition().y > WORLD_SIZE) {
                //std::cout << "  Bullet ID: " << ecsb->id << " expired or out of bounds, destroying entity.\n";
                entityManager.DestroyEntity(entity);
            }
        }
    }
};

class InputSystem : public ISystem {
public: 
    void Update(EntityManager& entityManager, float deltaTime) override {
        auto query = entityManager.CreateQuery<Transform, Playable, SpaceShip>();
        //std::cout << "InputSystem Update\n";
        for (auto [entity, transform, play, ship] : query) {
            int p = play->playerId;
            InputBlob input = play->input;
            uint8_t m = input.data[0];
            
            const float MOVE_SPEED = 1.0f;
            const float ROT_SPEED = 5.0f;
            const float BULLET_SPEED = 2.0f;
            const int SHOOT_COOLDOWN = 3;  // In ticks
            const int BULLET_LIFETIME = 30;  // In ticks

            // Decrease shoot cooldown
            if (ship->shootCooldown > 0) {
                ship->shootCooldown--;
            }

            if(ship->health==0) continue;
            
            // Rotation
            if (m & INPUT_LEFT) {
                transform->setRotation(transform->getRotation() + glm::vec3(0.0f, 0.0f, ROT_SPEED));
                if(transform->getRotation().z >= 360) transform->setRotation(transform->getRotation() + glm::vec3(0.0f, 0.0f, -360.0f));
            }
            if (m & INPUT_RIGHT) {
                transform->setRotation(transform->getRotation() + glm::vec3(0.0f, 0.0f, -ROT_SPEED));
                if(transform->getRotation().z < 0) transform->setRotation(transform->getRotation() + glm::vec3(0.0f, 0.0f, 360.0f));
            }
            
            // Movement
            float velX = 0, velY = 0;
            float radians = transform->getRotation().z * 3.14159f / 180.0f;
            
            if (m & INPUT_TOP) {
                velX += cos(radians) * MOVE_SPEED;
                velY += sin(radians) * MOVE_SPEED;
            }
            if (m & INPUT_DOWN) {
                velX -= cos(radians) * MOVE_SPEED;
                velY -= sin(radians) * MOVE_SPEED;
            }
            
            // Update position
            transform->setPosition(transform->getPosition() + glm::vec3(velX, velY, 0.0f));
            
            // Shooting
            if ((m & INPUT_SHOOT) && ship->shootCooldown <= 0) {
                // Find first available bullet ID
                bool usedIds[MAX_BULLETS] = {false};
                
                auto query2 = entityManager.CreateQuery<ECSBullet>();
                for (auto [entity, ecsb] : query2) {
                    if (ecsb->id >= 0 && ecsb->id < MAX_BULLETS) {
                        usedIds[ecsb->id] = true;
                    }
                }
                
                int id = -1;
                for (int i = 0; i < MAX_BULLETS; i++) {
                    if (!usedIds[i]) {
                        id = i;
                        break;
                    }
                }
                
                if (id != -1) {
                    //std::cout << "Player " << p << " shoots bullet ID " << id << " from (" << transform->getPosition().x << ", " << transform->getPosition().y << ") with rotation " << transform->getRotation().z << "\n";
                    
                    // Create bullet entity
                    Entity bulletEntity = entityManager.CreateEntity();
                    Transform* t = entityManager.AddComponent<Transform>(bulletEntity, Transform{});
                    t->setPosition(glm::vec3(transform->getPosition().x, transform->getPosition().y, 0.0f));

                    float bVelX = cos(radians) * BULLET_SPEED;
                    float bVelY = sin(radians) * BULLET_SPEED;
                    entityManager.AddComponent<ECSBullet>(bulletEntity, ECSBullet{id, bVelX, bVelY, p, BULLET_LIFETIME});
                    BoxCollider2D* collider = entityManager.AddComponent<BoxCollider2D>(bulletEntity, BoxCollider2D{glm::vec2(1.0f, 1.0f)});
                    collider->layer = CollisionLayer::BULLET;
                    collider->collidesWith = CollisionLayer::PLAYER;

                    // Capture pointer to EntityManager to avoid copying it into the lambda (and const-qualification issues)
                    collider->SetOnCollisionEnter([this, em = &entityManager](Entity self, Entity other, const CollisionInfo& info) {
                        auto bullet = em->GetComponent<ECSBullet>(self);
                        auto player = em->GetComponent<Playable>(other);
                        auto ship = em->GetComponent<SpaceShip>(other);

                        // Only apply damage if player is alive
                        if (bullet && player && ship && bullet->ownerId != player->playerId && ship->health > 0) {
                            auto col = em->GetComponent<BoxCollider2D>(other);
                            
                            ship->health -= 5;

                            if(ship->health <= 0) {
                                ship->health = 0;
                                ship->deathCooldown = 10 * TICKS_PER_SECOND;
                                col->isEnabled = false;
                            }

                            em->DestroyEntity(self);  // Destroy bullet after damage
                        }
                    });
                    
                    // Set cooldown
                    ship->shootCooldown = SHOOT_COOLDOWN;
                }
            }
        }
    }
};

class CameraFollowSystem : public ISystem {
public:
    void Update(EntityManager& entityManager, float deltaTime) override {
        // Query cameras with their transforms
        auto camQuery = entityManager.CreateQuery<Camera, Transform>();
        if (camQuery.Count() == 0) return; // No camera found

        // Find the local player position (if any)
        glm::vec3 localPos(0.0f);
        bool foundLocal = false;
        auto playerQuery = entityManager.CreateQuery<Transform, Playable, SpaceShip>();

        for (auto [entity, playerTransform, play,ship] : playerQuery) {
            if (play->isLocal) {
                localPos = playerTransform->getPosition();
                foundLocal = true;

                auto textQuery = entityManager.CreateQuery<UIElement,UIText>();

                for(auto [entity, element, text] : textQuery)
                {
                    element->anchor = UIAnchor::TOP_LEFT;
                    element->position = glm::vec2(0.0f, 0.0f);  
                    element->size = glm::vec2(150.0f, 40.0f);
                    element->pivot = glm::vec2(0.0f, 0.0f);
                    text->text = "Health: " + std::to_string(ship->health);
                }


                break;
            }
        }

        // Update each camera to follow the local player smoothly
        const float followSpeed = 8.0f; // higher = snappier, lower = smoother
        for (auto [camEntity, cam, camTrans] : camQuery) {
            if (!foundLocal) break; // nothing to follow

            // Desired camera position preserves Z but matches player's X/Y
            glm::vec3 desiredPos(localPos.x, localPos.y, camTrans->getPosition().z);

            // Let Transform perform smoothing (same exponential smoothing math)
            camTrans->SmoothPositionToward(desiredPos, deltaTime, followSpeed);

            // Smooth camera target toward the player
            glm::vec3 currentTarget = cam->getTarget();
            float t = 1.0f - std::exp(-followSpeed * deltaTime);
            glm::vec3 newTarget = currentTarget + (localPos - currentTarget) * t;
            cam->setTarget(newTarget);
            cam->markViewDirty();
        }
    }
};

class OnDeathLogicSystem : public ISystem {
public:
    void Update(EntityManager& entityManager, float deltaTime) override {
        bool foundLocal = false;
        auto playerQuery = entityManager.CreateQuery<Transform, Playable, SpaceShip, BoxCollider2D>();

        for (auto [entity, playerTransform, play,ship, col] : playerQuery) {
            if(ship->health==0)
            {
                ship->deathCooldown--;

                if(ship->deathCooldown<=0)
                {
                    ship->deathCooldown = 0;
                    ship->health = 100;
                    playerTransform->setPosition(glm::vec3(0.0f, 0.0f, 0.0f));
                    playerTransform->setRotation(glm::vec3(0.0f, 0.0f, 0.0f));
                    col->isEnabled = true;
                }
            }
            else
            {
                col->isEnabled = true;
            }
            
        }
    }
};

class OnDeathRenderSystem : public ISystem {
public:
    void Update(EntityManager& entityManager, float deltaTime) override {
        bool foundLocal = false;
        auto playerQuery = entityManager.CreateQuery<Transform, Playable, SpaceShip, MeshComponent>();

        for (auto [entity, playerTransform, play,ship,meshC] : playerQuery) {
            if (play->isLocal) {
                if(ship->health==0&&ship->deathCooldown>0)
                {
                    auto textQuery = entityManager.CreateQuery<UIElement,UIText>();

                    for(auto [entity, element, text] : textQuery)
                    {
                        element->anchor = UIAnchor::CENTER;
                        element->position = glm::vec2(0.0f, 0.0f);  
                        element->size = glm::vec2(150.0f, 40.0f);
                        element->pivot = glm::vec2(0.5f, 0.5f);

                        int seconds_remain = ship->deathCooldown / TICKS_PER_SECOND;



                        text->text = std::to_string(seconds_remain+1);
                    }
                }

            }

            if(ship->health<=0)
            {
                meshC->enabled = false;
            }
            else
            {
                meshC->enabled = true;
            }
        }
    }
};



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
            printf("    Rotation: %d degrees\n", state.rot[i]);
            printf("    Health: %d\n", state.health[i]);
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
            s.rot[p] = static_cast<int>(transform->getRotation().z);
            s.shootCooldown[p] = ship->shootCooldown;
            s.health[p] = ship->health;
            s.deathCooldown[p] = ship->deathCooldown;
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
    }

    void GameState_To_ECSWorld(const GameStateBlob& state, std::map<int, InputEntry> inputs) {
        AsteroidShooterGameState s = *reinterpret_cast<const AsteroidShooterGameState*>(state.data);

        auto query = world.GetEntityManager().CreateQuery<Transform, Playable, SpaceShip>();
        for (auto [entity, transform, play, ship] : query) {
            int p = play->playerId;
            transform->setPosition(glm::vec3(s.posX[p], s.posY[p], 0.0f));
            transform->setRotation(glm::vec3(0.0f, 0.0f, static_cast<float>(s.rot[p])));
            ship->shootCooldown = s.shootCooldown[p];
            ship->health = s.health[p];
            ship->deathCooldown = s.deathCooldown[p];
            // Update input
            auto it = inputs.find(p);
            if (it != inputs.end()) {
                play->input = it->second.input;
            }
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
                    world.GetEntityManager().AddComponent<ECSBullet>(newBullet, ECSBullet{b.id, b.velX, b.velY, b.ownerId, b.lifetime});
                }
            }
        }
    }

    bool CompareStates(const GameStateBlob& a, const GameStateBlob& b) const override {
        return std::memcmp(a.data, b.data, sizeof(AsteroidShooterGameState)) == 0;
    }

    void InitECSLogic(GameStateBlob& state) override {
        AsteroidShooterGameState s;
        std::memset(&s, 0, sizeof(AsteroidShooterGameState));
        
        // Initialize players
        s.posX[0] = -50; s.posY[0] = -50;
        s.posX[1] =  50; s.posY[1] =  50;
        s.rot[0] = 0; s.rot[1] = 180;
        
        // Initialize bullets (all inactive)
        for (int i = 0; i < MAX_BULLETS; i++) {
            s.bullets[i].id = -1;
            s.bullets[i].active = false;
        }
        s.bulletCount = 0;
        
        // Initialize cooldowns
        s.shootCooldown[0] = 0;
        s.shootCooldown[1] = 0;

        s.health[0] = 100;
        s.health[1] = 100;

        s.deathCooldown[0] = 0;
        s.deathCooldown[1] = 0;
        
        std::memset(state.data, 0, sizeof(state.data));
        std::memcpy(state.data, &s, sizeof(AsteroidShooterGameState));

        state.len = sizeof(AsteroidShooterGameState);

        world.GetEntityManager().RegisterComponentType<SpaceShip>();
        world.GetEntityManager().RegisterComponentType<ECSBullet>();

        Entity player1 = world.GetEntityManager().CreateEntity();
        Transform* t1 = world.GetEntityManager().AddComponent<Transform>(player1, Transform{});
        t1->setPosition(glm::vec3(s.posX[0], s.posY[0], 0.0f));
        t1->setRotation(glm::vec3(0.0f, 0.0f, 0.0f));

        world.GetEntityManager().AddComponent<Playable>(player1, Playable{0, MakeZeroInputBlob(), (0 == playerId ? true : false)});
        world.GetEntityManager().AddComponent<SpaceShip>(player1, SpaceShip{100,0,0});
        BoxCollider2D* collider1 = world.GetEntityManager().AddComponent<BoxCollider2D>(player1, BoxCollider2D{glm::vec2(1.5f, 3.0f)});
        collider1->layer = CollisionLayer::PLAYER;
        collider1->collidesWith = CollisionLayer::BULLET;

        Entity player2 = world.GetEntityManager().CreateEntity();
        Transform* t2 = world.GetEntityManager().AddComponent<Transform>(player2, Transform{});
        t2->setPosition(glm::vec3(s.posX[1], s.posY[1], 0.0f));
        t2->setRotation(glm::vec3(0.0f, 0.0f, 180.0f));
        
        world.GetEntityManager().AddComponent<Playable>(player2, Playable{1, MakeZeroInputBlob(), (1 == playerId ? true : false)});
        world.GetEntityManager().AddComponent<SpaceShip>(player2, SpaceShip{100,0,0});
        BoxCollider2D* collider2 = world.GetEntityManager().AddComponent<BoxCollider2D>(player2, BoxCollider2D{glm::vec2(1.5f, 3.0f)});
        collider2->layer = CollisionLayer::PLAYER;
        collider2->collidesWith = CollisionLayer::BULLET;
        
        world.AddSystem(std::make_unique<InputSystem>());
        world.AddSystem(std::make_unique<BulletSystem>());
        world.AddSystem(std::make_unique<OnDeathLogicSystem>());
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
            transform->setRotation(glm::vec3(0.0f, 0.0f, static_cast<float>(s.rot[p])));

            ship->shootCooldown = s.shootCooldown[p];
            ship->health = s.health[p];
            ship->deathCooldown = s.deathCooldown[p];
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
        

        Entity player1 = world.GetEntityManager().CreateEntity();
        Transform* t1 = world.GetEntityManager().AddComponent<Transform>(player1, Transform{});
        t1->setPosition(glm::vec3(s.posX[0], s.posY[0], 0.0f));
        t1->setRotation(glm::vec3(0.0f, 0.0f, 0.0f));
        
        world.GetEntityManager().AddComponent<Playable>(player1, Playable{0, MakeZeroInputBlob(), (0 == playerId ? true : false)});
        world.GetEntityManager().AddComponent<SpaceShip>(player1, SpaceShip{100,0,0});
        world.GetEntityManager().AddComponent<MeshComponent>(player1, MeshComponent(new Mesh(TriangleVerts(), triangleInds, red)));

        Entity player2 = world.GetEntityManager().CreateEntity();
        Transform* t2 = world.GetEntityManager().AddComponent<Transform>(player2, Transform{});
        t2->setPosition(glm::vec3(s.posX[1], s.posY[1], 0.0f));
        t2->setRotation(glm::vec3(0.0f, 0.0f, 180.0f));
        
        world.GetEntityManager().AddComponent<Playable>(player2, Playable{1, MakeZeroInputBlob(), (1 == playerId ? true : false)});
        world.GetEntityManager().AddComponent<SpaceShip>(player2, SpaceShip{100,0,0});
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
    }

    ~AsteroidShooterGameRenderer() override {
        //
    }

private:
    OpenGLWindow* window;  // Store window pointer for cleanup
};

#endif