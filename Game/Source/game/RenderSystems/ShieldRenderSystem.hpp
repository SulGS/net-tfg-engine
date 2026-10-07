#pragma once

#include <algorithm>

#include "ecs/ecs_common.hpp"
#include "OpenAL/AudioComponents.hpp"
#include "../Components.hpp"
#include "LaserVisuals.hpp"

// Laser shield bubble (laser_shield.frag): one entity per raised shield, created when SpaceShip::shieldTicks goes above 0
// and kept for a moment after it drops back to 0, to shatter (broken by a bullet) or fade out (expired, or the ship died
// to something the shield doesn't stop). Purely visual: the shield itself is the replicated shieldTicks.
class ShieldRenderSystem : public ISystem
{
    const float SPIN_UP_SECONDS = 0.25f;
    const float BREAK_SECONDS = 0.45f;
    const float FADE_SECONDS = 0.25f;
    const int   EXPIRE_WARN_TICKS = 45;   // last 1.5 s: stutter
    // Natural expiry walks shieldTicks down 1 by 1, so the last value a frame sees before 0 is ~1-2 (a bit more below
    // 30 fps). Anything above this dropped straight to 0: a bullet broke it.
    const int   BREAK_MIN_LAST_TICKS = 4;
    float time = 0.0f;

    // shield.wav (~1.7 s) once, where the shield went up. Its own short-lived entity like the shot sound (see
    // AsteroidShooterGameRenderer::GameState_To_ECSWorld): DestroyTimerSystem releases it once it has played.
    static void PlayRaiseSound(EntityManager& entityManager, const glm::vec3& position)
    {
        Entity soundEntity = entityManager.CreateEntity();
        Transform* t = entityManager.AddComponent<Transform>(soundEntity, Transform{});
        t->setPosition(position);
        entityManager.AddComponent<DestroyTimer>(soundEntity, DestroyTimer{ 2.5f });
        AudioSourceComponent* audio = entityManager.AddComponent<AudioSourceComponent>(
            soundEntity, AudioSourceComponent("shield.wav", AudioChannel::SFX, false));
        audio->play = true;
        audio->gain = 1.0f;
    }

    void Push(MeshComponent* mesh, float power, float expire, float breakT, float fade) const
    {
        if (!mesh->mesh) return;
        if (Material* mat = mesh->mesh->getMaterial())
        {
            mat->setFloat("uTime", time);
            mat->setFloat("uPower", power);
            mat->setFloat("uExpire", expire);
            mat->setFloat("uBreak", breakT);
            mat->setFloat("uFade", fade);
        }
    }

public:
    void Update(EntityManager& entityManager, std::vector<EventEntry>& events, bool isServer, float deltaTime) override
    {
        time += deltaTime;

        auto shipQuery = entityManager.CreateQuery<Transform, Playable, SpaceShip>();
        auto shieldQuery = entityManager.CreateQuery<Transform, ShieldEffect, MeshComponent>();

        for (auto [shipEntity, shipTransform, play, ship] : shipQuery)
        {
            const bool up = ship->isAlive && ship->shieldTicks > 0;
            bool found = false;

            for (auto [shieldEntity, shieldTransform, shield, mesh] : shieldQuery)
            {
                if (shield->playerId != play->playerId) continue;
                found = true;

                const glm::vec3 shipPos = shipTransform->getPosition();
                shieldTransform->setPosition(glm::vec3(shipPos.x, shipPos.y, 0.0f));

                // Raised again while the previous bubble was still going away: take it over.
                if (up && shield->fadeOut >= 0.0f)
                {
                    PlayRaiseSound(entityManager, shieldTransform->getPosition());
                    shield->fadeOut = -1.0f;
                    shield->broken = false;
                    shield->power = 0.0f;
                }

                if (!up && shield->fadeOut < 0.0f)
                {
                    shield->fadeOut = 0.0f;
                    shield->broken = ship->isAlive && shield->lastTicks > BREAK_MIN_LAST_TICKS;
                }

                PointLightComponent* light = entityManager.GetComponent<PointLightComponent>(shieldEntity);

                if (up)
                {
                    shield->power = std::min(shield->power + deltaTime / SPIN_UP_SECONDS, 1.0f);
                    shield->lastTicks = ship->shieldTicks;
                    const float expire = (ship->shieldTicks < EXPIRE_WARN_TICKS)
                        ? 1.0f - static_cast<float>(ship->shieldTicks) / EXPIRE_WARN_TICKS
                        : 0.0f;
                    Push(mesh, shield->power, expire, 0.0f, 1.0f);
                    if (light) light->intensity = SHIELD_LIGHT_INTENSITY * shield->power;
                    continue;
                }

                shield->fadeOut += deltaTime;
                const float duration = shield->broken ? BREAK_SECONDS : FADE_SECONDS;
                const float t = std::clamp(shield->fadeOut / duration, 0.0f, 1.0f);
                if (t >= 1.0f)
                {
                    entityManager.DestroyEntity(shieldEntity);
                    continue;
                }

                if (shield->broken)
                {
                    // Bright burst first, then it goes dark as it shatters.
                    Push(mesh, shield->power, 0.0f, std::max(t, 0.001f), 1.0f);
                    if (light) light->intensity = SHIELD_LIGHT_INTENSITY * 3.0f * (1.0f - t) * (1.0f - t);
                }
                else
                {
                    Push(mesh, shield->power, 0.0f, 0.0f, 1.0f - t);
                    if (light) light->intensity = SHIELD_LIGHT_INTENSITY * (1.0f - t);
                }
            }

            if (!found && up)
            {
                Entity shieldEntity = entityManager.CreateEntity();
                Transform* t = entityManager.AddComponent<Transform>(shieldEntity, Transform{});
                const glm::vec3 shipPos = shipTransform->getPosition();
                t->setPosition(glm::vec3(shipPos.x, shipPos.y, 0.0f));
                t->setScale(glm::vec3(SHIELD_RADIUS));

                ShieldEffect* shield = entityManager.AddComponent<ShieldEffect>(shieldEntity, ShieldEffect{ play->playerId });
                shield->lastTicks = ship->shieldTicks;

                MeshComponent* mesh = AddShieldMesh(entityManager, shieldEntity, play->playerId);
                AddShieldLight(entityManager, shieldEntity);
                Push(mesh, 0.0f, 0.0f, 0.0f, 1.0f);
                PlayRaiseSound(entityManager, t->getPosition());
            }
        }
    }
};
