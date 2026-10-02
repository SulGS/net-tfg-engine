#pragma once

#include <algorithm>
#include <cmath>

#include "ecs/ecs_common.hpp"
#include "../Components.hpp"
#include "InputMask.hpp"

class InputSystem : public ISystem {
public:
    void Update(EntityManager& entityManager, std::vector<EventEntry>& events, bool isServer, float deltaTime) override {
        // Pre-match freeze: no movement/rotation/shooting until the countdown hits 0. Runs on client and server (MatchStartTimer
        // is synced), so skipping here keeps client prediction from drifting ahead during the freeze.
        {
            auto timerQuery = entityManager.CreateQuery<MatchStartTimer>();
            for (auto [entity, timer] : timerQuery)
                if (timer->ticksRemaining > 0) return;
        }

        auto query = entityManager.CreateQuery<Transform, Playable, SpaceShip>();
        for (auto [entity, transform, play, ship] : query) {
            InputBlob input = play->input;
            uint8_t m = input.data[0];

            const float ROT_THRUST = 1.5f;   // angular acceleration per tick (degrees)
            const float ROT_DRAG = 0.85f;  // angular velocity multiplier per tick
            const float MAX_ROT_SPEED = 8.0f;   // max degrees per tick
            const float THRUST = 0.15f;  // linear acceleration per tick
            const float DRAG = 0.92f;  // linear velocity multiplier per tick
            const float MAX_SPEED = 3.0f;   // max linear velocity magnitude
            const int   CHARGE_SHOOT_FRAMES = 5;

            if (!ship->isAlive) continue;

            if (!isServer) {
                if (ship->shootCooldown > 0) ship->shootCooldown--;
                if (ship->remainingShootFrames > 0) ship->remainingShootFrames--;
                if (ship->remainingShootFrames == 0) ship->isShooting = false;
            }

            bool notRotating = !(m & INPUT_LEFT) && !(m & INPUT_RIGHT);

            // Analog intensity (InputMask.hpp): 1.0 exactly for keys, so keyboard play is unchanged. A half-pushed
            // stick turns at half the rate and banks half as far (leaning back toward that bank if it was deeper).
            const float leftScale = DecodeInputIntensity(input.data[INPUT_BYTE_LEFT]);
            const float rightScale = DecodeInputIntensity(input.data[INPUT_BYTE_RIGHT]);
            const float thrustScale = DecodeInputIntensity(input.data[INPUT_BYTE_THRUST]);

            if (m & INPUT_LEFT)
            {
                ship->angularVel += ROT_THRUST * leftScale;
                const int bank = static_cast<int>(std::lround(40.0f * leftScale));
                ship->shipInclination = (ship->shipInclination < bank)
                    ? std::min(ship->shipInclination + 5, bank)
                    : std::max(ship->shipInclination - 3, bank);
            }
            if (m & INPUT_RIGHT)
            {
                ship->angularVel -= ROT_THRUST * rightScale;
                const int bank = -static_cast<int>(std::lround(40.0f * rightScale));
                ship->shipInclination = (ship->shipInclination > bank)
                    ? std::max(ship->shipInclination - 5, bank)
                    : std::min(ship->shipInclination + 3, bank);
            }
            if (notRotating)
            {
                if (ship->shipInclination > 0) ship->shipInclination = std::max(ship->shipInclination - 3, 0);
                else if (ship->shipInclination < 0) ship->shipInclination = std::min(ship->shipInclination + 3, 0);
            }

            // Angular drag and cap
            ship->angularVel *= ROT_DRAG;
            if (ship->angularVel > MAX_ROT_SPEED) ship->angularVel = MAX_ROT_SPEED;
            if (ship->angularVel < -MAX_ROT_SPEED) ship->angularVel = -MAX_ROT_SPEED;

            // Apply angular velocity to rotation
            float newZ = transform->getRotation().z + ship->angularVel;
            if (newZ >= 360.0f) newZ -= 360.0f;
            if (newZ < 0.0f) newZ += 360.0f;
            transform->setRotation(glm::vec3(transform->getRotation().x, transform->getRotation().y, newZ));

            // Thrust — uses updated rotation so direction matches visuals.
            // shipZRotation is the authoritative rotation for the state blob.
            ship->shipZRotation = static_cast<int>(newZ);

            float radians = newZ * 3.14159f / 180.0f;
            float fwdX = cos(radians);
            float fwdY = sin(radians);

            if (m & INPUT_TOP)
            {
                ship->velX += fwdX * THRUST * thrustScale;
                ship->velY += fwdY * THRUST * thrustScale;
                ship->isMovingForward = true;
            }
            else
            {
                ship->isMovingForward = false;
            }

            // Linear drag and speed cap
            ship->velX *= DRAG;
            ship->velY *= DRAG;

            float speed = sqrt(ship->velX * ship->velX + ship->velY * ship->velY);
            if (speed > MAX_SPEED)
            {
                float scale = MAX_SPEED / speed;
                ship->velX *= scale;
                ship->velY *= scale;
            }

            transform->setPosition(transform->getPosition() + glm::vec3(ship->velX, ship->velY, 0.0f));

            // Charging no longer freezes the ship (see InputServerSystem for the
            // muzzle-offset spawn that made that unnecessary) — just block
            // starting a *new* charge while one's already running.
            if ((m & INPUT_SHOOT) && !ship->isShooting && ship->shootCooldown <= 0 && ship->isAlive)
            {
                ship->remainingShootFrames = CHARGE_SHOOT_FRAMES;
                ship->isShooting = true;
            }
        }
    }
};
