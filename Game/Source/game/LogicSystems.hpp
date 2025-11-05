#pragma once

#include "netcode/netcode_common.hpp"
#include "ecs/ecs_common.hpp"
#include "Components.hpp"
#include "GameState.hpp"
#include "ecs/Collisions/BoxCollider2D.hpp"

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
    void Update(EntityManager& entityManager, std::vector<EventEntry>& events, float deltaTime) override {
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
    void Update(EntityManager& entityManager, std::vector<EventEntry>& events, float deltaTime) override {
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

            if (ship->health == 0) continue;

            // Rotation
            if (m & INPUT_LEFT) {
                transform->setRotation(transform->getRotation() + glm::vec3(0.0f, 0.0f, ROT_SPEED));
                if (transform->getRotation().z >= 360) transform->setRotation(transform->getRotation() + glm::vec3(0.0f, 0.0f, -360.0f));
            }
            if (m & INPUT_RIGHT) {
                transform->setRotation(transform->getRotation() + glm::vec3(0.0f, 0.0f, -ROT_SPEED));
                if (transform->getRotation().z < 0) transform->setRotation(transform->getRotation() + glm::vec3(0.0f, 0.0f, 360.0f));
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
        }
    }
};

class InputServerSystem : public ISystem {
public:
    void Update(EntityManager& entityManager, std::vector<EventEntry>& events, float deltaTime) override {
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

            float radians = transform->getRotation().z * 3.14159f / 180.0f;

			EventEntry posEvent;
			posEvent.event.type = AsteroidEventMask::PLAYER_POSITION;
			PlayerPositionEventData posData;

			posData.playerId = p;
			posData.x = transform->getPosition().x;
			posData.y = transform->getPosition().y;
			posData.rotation = transform->getRotation().z;
			std::memcpy(posEvent.event.data, &posData, sizeof(PlayerPositionEventData));
			posEvent.event.len = sizeof(PlayerPositionEventData);
			events.push_back(posEvent);

            // Shooting
            if ((m & INPUT_SHOOT) && ship->shootCooldown <= 0) {
                // Find first available bullet ID
                bool usedIds[MAX_BULLETS] = { false };

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

                  

                    // Set cooldown
                    ship->shootCooldown = SHOOT_COOLDOWN;
                }
            }
        }
    }
};

class OnDeathLogicSystem : public ISystem {
public:
    void Update(EntityManager& entityManager, std::vector<EventEntry>& events, float deltaTime) override {
        bool foundLocal = false;
        auto playerQuery = entityManager.CreateQuery<Transform, Playable, SpaceShip, BoxCollider2D>();

        for (auto [entity, playerTransform, play, ship, col] : playerQuery) {
            if (ship->health == 0)
            {
                ship->deathCooldown--;

                if (ship->deathCooldown <= 0)
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