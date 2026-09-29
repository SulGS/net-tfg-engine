#pragma once

#include <cmath>
#include <glm/glm.hpp>

#include "ecs/ecs_common.hpp"
#include "OpenGL/Particles/ParticleEmitterComponent.hpp"
#include "../Components.hpp"

class LinkThrusterToShipSystem : public ISystem
{
public:
    void Update(
        EntityManager& entityManager,
        std::vector<EventEntry>& events,
        bool isServer,
        float deltaTime
    ) override
    {
        auto thrusterQuery = entityManager.CreateQuery<Transform, ParticleEmitterComponent, ThrusterOwner>();
        auto shipQuery = entityManager.CreateQuery<Transform, Playable, SpaceShip>();

        for (auto [shipEntity, shipTransform, play, ship] : shipQuery)
        {
            if (!ship->isAlive) continue;
            float yawRad = glm::radians(shipTransform->getRotation().z);
            float incl = ship->shipInclination * -1.0f;
            glm::vec3 rotation;
            rotation.x = incl * cos(yawRad);
            rotation.y = incl * sin(yawRad);
            rotation.z = shipTransform->getRotation().z;
            shipTransform->setRotation(rotation);
        }

        for (auto [thrusterEntity, thrusterTransform, thrusterEmitter, thrusterOwner] : thrusterQuery)
        {
            for (auto [shipEntity, shipTransform, play, ship] : shipQuery)
            {
                if (thrusterOwner->shipEntity != play->playerId) continue;

                // Ship dead — kill emitters, skip everything else
                if (!ship->isAlive)
                {
                    thrusterEmitter->enabled = false;
                    break;
                }

                // Position (same for both thruster and smoke)
                glm::vec3 localOffset = glm::vec3(-1.8f, 0.0f, 0.0f);

                if (thrusterOwner->isLeftEngine)
                {
                    localOffset.y = -0.75f;
                }
                else
                {
                    localOffset.y = 0.75f;
                }




                glm::mat4 model = shipTransform->getModelMatrix();
                glm::mat3 rot = glm::mat3(model);
                rot[0] = glm::normalize(rot[0]);
                rot[1] = glm::normalize(rot[1]);
                rot[2] = glm::normalize(rot[2]);
                thrusterTransform->setPosition(shipTransform->getPosition() + rot * localOffset);


                if (thrusterOwner->isSmoke)
                {
                    thrusterEmitter->enabled = !ship->isMovingForward;  // smoke when idle
                    // Particle system reads local -Z, so this aligns it to world
                    // -Y (down), independent of the ship's heading, so idle smoke
                    // drifts downward under gravity instead of trailing backward.
                    thrusterTransform->setRotation(glm::vec3(
                        -90.0f,
                        0.0f,
                        0.0f
                    ));
                }
                else
                {
                    thrusterEmitter->enabled = ship->isMovingForward;   // exhaust when moving
                    thrusterTransform->setRotation(glm::vec3(
                        90.0f,
                        shipTransform->getRotation().z,
                        0.0f
                    ));
                }

            }
        }
    }
};
