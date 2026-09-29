#pragma once

#include <cmath>
#include <cstring>

#include "ecs/ecs_common.hpp"
#include "../Components.hpp"
#include "../Events.hpp"
#include "../GameState.hpp"

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
                        // Spawn at the nose (SHIP_MUZZLE_OFFSET) with the ship's *current* rotation/position when the charge
                        // finishes — the same point the charge-up orb was visually sitting at.
                        spawnData.bulletId = id;
                        spawnData.ownerId = p;
                        spawnData.posX = transform->getPosition().x + cos(radians) * SHIP_MUZZLE_OFFSET;
                        spawnData.posY = transform->getPosition().y + sin(radians) * SHIP_MUZZLE_OFFSET;
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
