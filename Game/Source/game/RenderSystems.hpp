#pragma once
#include <unordered_set>
#include <algorithm>
#include <cmath>

#include "netcode/netcode_common.hpp"
#include "Utils/Input.hpp"
#include "ecs/ecs_common.hpp"
#include "ecs/UI/UIButton.hpp"
#include "ecs/UI/UIImage.hpp"
#include "ecs/UI/UIText.hpp"
#include "ecs/UI/UIElement.hpp"
#include "Components.hpp"
#include "GameState.hpp"
#include "Explosion.hpp"
#include "NetTFG_Engine.hpp"

// Returns the playerId of the sole surviving player, or -1 if the game is
// still ongoing (0 or 2+ players alive).
inline int GetWinnerId(EntityManager& entityManager)
{
    int aliveCount = 0;
    int winnerId = -1;
    int total = 0;

    auto q = entityManager.CreateQuery<Playable, SpaceShip>();
    for (auto [e, play, ship] : q)
    {
        total++;
        if (ship->isAlive) { aliveCount++; winnerId = play->playerId; }
    }

    return (total >= 2 && aliveCount == 1) ? winnerId : -1;
}

// ---------------------------------------------------------------------------
// Laser shot + charge-up visuals
//
// Both are volumetric glows raymarched inside a "canvas" mesh, drawn additively
// (MeshComponent::additive). The canvas is a UNIT SPHERE (charge.glb) that the
// entity's scale stretches into the volume the effect needs: a long ellipsoid for
// the bolt, a sphere for the charge orb. See glow_volume.vert.
// ---------------------------------------------------------------------------
inline constexpr const char* GLOW_VOLUME_MESH = "charge.glb";

// Bolt canvas: local +X is the direction of flight (the bullet is rotated about Z
// to face its velocity). The head sits ~0.7 ahead of the bullet's position, i.e.
// on its 2x2 collision box, and the tail trails ~7 units behind it — a bit more
// than the 5 units a bullet travels per tick, so the streak always covers the
// distance covered since the last tick.
inline const glm::vec3 LASER_BOLT_SCALE(7.0f, 1.7f, 1.7f);

// The light a bolt casts on what it flies over (tiles, pillars, ships). It is one of
// the very few point lights in the scene, so it does the visible work. A bolt flies
// ~4 above the tiles: with these values the floor right under it gets ~3x its
// albedo, ~10 units away ~0.4x, and nothing beyond the radius.
inline constexpr float LASER_BOLT_LIGHT_INTENSITY = 150.0f;
inline constexpr float LASER_BOLT_LIGHT_RADIUS = 40.0f;
inline const glm::vec3 LASER_BOLT_LIGHT_COLOR(1.0f, 0.6f, 0.2f);

// Charge orb: radius of the canvas sphere (the visible glow is smaller than this).
// How far ahead of the ship's centre it forms (its nose) is SHIP_MUZZLE_OFFSET
// (Components.hpp), shared with InputServerSystem so the bolt picks up exactly
// where the orb visually left off.
inline constexpr float CHARGE_ORB_RADIUS = 4.2f;

// Adds the laser bolt's mesh to a bullet entity. One Material per bolt: each
// carries its own age/fade uniforms (BulletRenderSystem); the compiled program is
// shared through ShaderLoader's cache. The colours are light being ADDED to the
// scene, not a surface colour.
inline MeshComponent* AddLaserBoltMesh(EntityManager& em, Entity bullet, int bulletId)
{
    auto mat = std::make_shared<Material>("glow_volume.vert", "laser_bolt.frag");
    mat->setVec3("uColor", glm::vec3(1.0f, 0.55f, 0.08f));
    mat->setVec3("uHotColor", glm::vec3(1.0f, 0.92f, 0.6f));
    mat->setFloat("uGlowStrength", 1.8f);
    mat->setFloat("uCoreStrength", 3.2f);
    mat->setFloat("uSeed", bulletId * 2.7f); // desyncs the tail noise between bolts

    MeshComponent* mc = em.AddComponent<MeshComponent>(bullet,
        MeshComponent(new Mesh(GLOW_VOLUME_MESH, mat)));
    mc->castShadows = false;
    mc->additive = true;
    return mc;
}

// Adds the bolt's point light to a bullet entity. It follows the entity's Transform;
// BulletRenderSystem scales its intensity every frame with the bolt's age and fade,
// so it starts at 0 here. It casts no shadows on purpose: the shadow cube-map slots
// are few (8) and expensive, and PointLightComponent defaults to casting them.
inline PointLightComponent* AddLaserBoltLight(EntityManager& em, Entity bullet)
{
    PointLightComponent* light =
        em.AddComponent<PointLightComponent>(bullet, PointLightComponent{});
    light->color = LASER_BOLT_LIGHT_COLOR;
    light->intensity = 0.0f;
    light->radius = LASER_BOLT_LIGHT_RADIUS;
    light->castShadows = false;
    return light;
}

// Same for the charge-up orb of a ship (seed = its player id).
inline MeshComponent* AddChargeOrbMesh(EntityManager& em, Entity orb, int playerId)
{
    auto mat = std::make_shared<Material>("glow_volume.vert", "laser_charge.frag");
    mat->setVec3("uColor", glm::vec3(1.0f, 0.55f, 0.08f));
    mat->setVec3("uHotColor", glm::vec3(1.0f, 0.92f, 0.6f));
    mat->setFloat("uGlowStrength", 1.8f);
    mat->setFloat("uCoreStrength", 3.2f);
    mat->setFloat("uSeed", playerId * 3.1f);

    MeshComponent* mc = em.AddComponent<MeshComponent>(orb,
        MeshComponent(new Mesh(GLOW_VOLUME_MESH, mat)));
    mc->castShadows = false;
    mc->additive = true;
    return mc;
}

class CameraFollowSystem : public ISystem
{
public:
    float debugZoomMultiplier = 1.25f; // 1.0 = normal, >1 = zoom out

    void Update(
        EntityManager& entityManager,
        std::vector<EventEntry>& events,
        bool isServer,
        float deltaTime
    ) override
    {
        auto buttonQuery = entityManager.CreateQuery<UIElement, UIButton>();

        for (auto [entity, element, button] : buttonQuery)
        {
            element->isVisible = false;
        }

        auto camQuery = entityManager.CreateQuery<Camera, Transform>();
        if (camQuery.Count() == 0)
            return;

        bool localAlive = false;
        bool localFound = false;
        int  localPlayerId = -1;
        glm::vec3 localPos(0.0f);

        auto playerQuery = entityManager.CreateQuery<Transform, Playable, SpaceShip>();
        for (auto [entity, playerTransform, play, ship] : playerQuery)
        {
            if (play->isLocal)
            {
                localFound = true;
                localPlayerId = play->playerId;
                localAlive = ship->isAlive;
                localPos = playerTransform->getPosition();
                break;
            }
        }

        if (!localFound)
            return;

        glm::vec3 targetPos(0.0f);

        // Game over: all players see the winner zoom regardless of alive state
        int winnerId = GetWinnerId(entityManager);
        bool gameOver = (winnerId >= 0);

        if (gameOver)
        {
            for (auto [e2, pt2, pl2, sh2] : playerQuery)
            {
                if (pl2->playerId == winnerId)
                {
                    targetPos = pt2->getPosition();
                    break;
                }
            }

            // Update UI for the alive winner (dead players are handled by OnDeathRenderSystem)
            if (localAlive)
            {
                auto textQuery = entityManager.CreateQuery<UIElement, UIText, GameStatusText>();
                for (auto [entity, element, text, statusTag] : textQuery)
                {
                    element->anchor = UIAnchor::TOP_CENTER;
                    element->position = glm::vec2(0.0f, 20.0f);
                    element->size = glm::vec2(350.0f, 80.0f);
                    element->pivot = glm::vec2(0.5f);
                    text->text = "PLAYER " + std::to_string(winnerId + 1) + " WINS";
                }

				auto buttonQuery = entityManager.CreateQuery<UIElement, UIButton>();

				for (auto [entity, element, button] : buttonQuery)
				{
					element->isVisible = true;
				}
            }

            float zoom = 0.6f;
            for (auto [camEntity, cam, camTrans] : camQuery)
            {
                glm::vec3 newCamPos(
                    targetPos.x,
                    targetPos.y - 27.0f * zoom,
                    18.0f * zoom
                );
                camTrans->setPosition(newCamPos);
                cam->setTarget(targetPos);
                cam->markViewDirty();
            }
            return;
        }

        // Normal gameplay
        if (localAlive)
        {
            targetPos = localPos;

            int aliveCount = 0;
            for (auto [e2, pt2, pl2, sh2] : playerQuery)
                if (sh2->isAlive) aliveCount++;

            auto textQuery = entityManager.CreateQuery<UIElement, UIText, GameStatusText>();
            for (auto [entity, element, text, statusTag] : textQuery)
            {
                element->anchor = UIAnchor::TOP_LEFT;
                element->position = glm::vec2(0.0f, 0.0f);
                element->size = glm::vec2(300.0f, 40.0f);
                element->pivot = glm::vec2(0.0f, 0.0f);
                text->text = "REMAINING: " + std::to_string(aliveCount);
            }
        }
        else
        {
            // Spectating: find the SpectatorState and follow the watched player
            SpectatorState* spectator = nullptr;
            for (auto [entity, playerTransform, play, ship] : playerQuery)
            {
                if (play->isLocal)
                {
                    spectator = entityManager.GetComponent<SpectatorState>(entity);

                    // Lazily add SpectatorState if missing
                    if (!spectator)
                    {
                        int firstAlive = -1;
                        for (auto [e2, pt2, pl2, sh2] : playerQuery)
                        {
                            if (pl2->playerId != localPlayerId && sh2->isAlive)
                            {
                                firstAlive = pl2->playerId;
                                break;
                            }
                        }
                        SpectatorState newState;
                        newState.watchedPlayerId = (firstAlive >= 0) ? firstAlive : localPlayerId;
                        spectator = entityManager.AddComponent<SpectatorState>(entity, newState);
                    }

                    // Cycle target with LEFT / RIGHT arrow keys — read hardware
                    // directly here since this is renderer-only local state and
                    // must never touch the networked input blob.
                    bool leftNow = Input::KeyPressed(Input::ArrowLeft);
                    bool rightNow = Input::KeyPressed(Input::ArrowRight);

                    auto cycleTarget = [&](int direction)
                        {
                            std::vector<int> alive;
                            for (auto [e2, pt2, pl2, sh2] : playerQuery)
                                if (pl2->playerId != localPlayerId && sh2->isAlive)
                                    alive.push_back(pl2->playerId);

                            if (alive.empty()) return;

                            auto it = std::find(alive.begin(), alive.end(), spectator->watchedPlayerId);
                            int idx = (it != alive.end()) ? (int)(it - alive.begin()) : 0;
                            idx = (idx + direction + (int)alive.size()) % (int)alive.size();
                            spectator->watchedPlayerId = alive[idx];
                        };

                    if (leftNow && !spectator->prevLeftHeld)  cycleTarget(-1);
                    if (rightNow && !spectator->prevRightHeld) cycleTarget(+1);

                    spectator->prevLeftHeld = leftNow;
                    spectator->prevRightHeld = rightNow;

                    break;
                }
            }

            // Follow the watched player (or stay put if everyone is dead)
            bool watchedFound = false;
            if (spectator)
            {
                for (auto [entity, playerTransform, play, ship] : playerQuery)
                {
                    if (play->playerId == spectator->watchedPlayerId)
                    {
                        targetPos = playerTransform->getPosition();
                        watchedFound = true;
                        break;
                    }
                }
            }
            if (!watchedFound)
                targetPos = localPos;
        }

        for (auto [camEntity, cam, camTrans] : camQuery)
        {
            glm::vec3 newCamPos(
                targetPos.x,
                targetPos.y - 27.0f * debugZoomMultiplier,
                18.0f * debugZoomMultiplier
            );
            camTrans->setPosition(newCamPos);
            cam->setTarget(targetPos);
            cam->markViewDirty();
        }
    }
};

// Overrides the shared GameStatusText label with a centred countdown while
// the pre-match freeze (MatchStartTimer, see LogicSystems.hpp) is active.
// Must be added AFTER CameraFollowSystem so it wins the frame's last write;
// once the countdown reaches 0 it does nothing, leaving that system's
// REMAINING/health text uncontested from then on.
class MatchStartCountdownRenderSystem : public ISystem
{
public:
    void Update(EntityManager& entityManager, std::vector<EventEntry>& events,
        bool isServer, float deltaTime) override
    {
        int ticksRemaining = 0;
        {
            auto timerQuery = entityManager.CreateQuery<MatchStartTimer>();
            for (auto [entity, timer] : timerQuery) ticksRemaining = timer->ticksRemaining;
        }
        if (ticksRemaining <= 0) return;

        int secondsRemaining = (ticksRemaining + TICKS_PER_SECOND - 1) / TICKS_PER_SECOND;

        auto textQuery = entityManager.CreateQuery<UIElement, UIText, GameStatusText>();
        for (auto [entity, element, text, statusTag] : textQuery)
        {
            element->anchor = UIAnchor::TOP_CENTER;
            element->position = glm::vec2(0.0f, 20.0f);
            element->size = glm::vec2(350.0f, 80.0f);
            element->pivot = glm::vec2(0.5f);
            text->text = std::to_string(secondsRemaining);
        }
    }
};


class OnDeathRenderSystem : public ISystem
{
public:
    void Update(
        EntityManager& entityManager,
        std::vector<EventEntry>& events,
        bool isServer,
        float deltaTime
    ) override
    {
        auto playerQuery =
            entityManager.CreateQuery<Transform, Playable, SpaceShip, MeshComponent, JustDeathChecker>();

        for (auto [entity, playerTransform, play, ship, meshC, jdC] : playerQuery)
        {

            if (!ship->isAlive)
            {
                if (jdC->notExecuted) {

                    jdC->notExecuted = false;


                    SpawnShipExplosion(entityManager, playerTransform->getPosition(), play->playerId);

                    Entity audioEntity = entityManager.CreateEntity();
					Transform* audioTransform = entityManager.AddComponent<Transform>(audioEntity, Transform{});
					audioTransform->setPosition(playerTransform->getPosition());
                    AudioSourceComponent* audio = entityManager.AddComponent<AudioSourceComponent>(
                        audioEntity, AudioSourceComponent("explosion.wav", AudioChannel::SFX, false));
					audio->gain = 1.0f;
                    audio->play = true;
                    entityManager.AddComponent<ExplosionPlayerID>(audioEntity, ExplosionPlayerID{ play->playerId });
                }
            }
            else
            {
				jdC->notExecuted = true; // reset for potential future deaths

				auto explosionQuery = entityManager.CreateQuery<Transform, ParticleEmitterComponent, ExplosionPlayerID>();
                for (auto [e, t, emitter, expID] : explosionQuery)
                {
                    if (expID->playerId == play->playerId)
                    {
                        entityManager.DestroyEntity(e);
                    }
                }

				auto audioQuery = entityManager.CreateQuery<AudioSourceComponent, ExplosionPlayerID>();
				for (auto [e, audio, expID] : audioQuery)
				{
					if (expID->playerId == play->playerId)
					{
						audio->pendingToDestroy = true;
					}
				}
            }


            if (play->isLocal && !ship->isAlive)
            {

                int winnerId = GetWinnerId(entityManager);
                bool gameOver = (winnerId >= 0);

                // Find who we're spectating (only needed when game is still ongoing)
                std::string watchedName = "";
                if (!gameOver)
                {
                    SpectatorState* spectator = entityManager.GetComponent<SpectatorState>(entity);
                    if (spectator)
                    {
                        auto allPlayers = entityManager.CreateQuery<Playable, SpaceShip>();
                        for (auto [e2, pl2, sh2] : allPlayers)
                        {
                            if (pl2->playerId == spectator->watchedPlayerId)
                            {
                                watchedName = "Player " + std::to_string(spectator->watchedPlayerId + 1);
                                break;
                            }
                        }
                    }
                }

                auto buttonQuery = entityManager.CreateQuery<UIElement, UIButton>();

                for (auto [entity, element, button] : buttonQuery)
                {
                    element->isVisible = true;
                }

                auto textQuery = entityManager.CreateQuery<UIElement, UIText, GameStatusText>();
                for (auto [uiEntity, element, text, statusTag] : textQuery)
                {
                    element->anchor = UIAnchor::TOP_CENTER;
                    element->position = glm::vec2(0.0f, 20.0f);
                    element->size = glm::vec2(350.0f, 80.0f);
                    element->pivot = glm::vec2(0.5f);

                    if (gameOver)
                        text->text = "PLAYER " + std::to_string(winnerId + 1) + " WINS";
                    else if (!watchedName.empty())
                        text->text = "SPECTATING " + watchedName;
                    else
                        text->text = "YOU DIED";
                }
            }

            meshC->enabled = ship->isAlive;
        }
    }
};

// The charge-up before a shot (laser_charge.frag): an orb of energy that forms at
// the ship's muzzle for as long as the ship is charging, with a shell of sparks
// that contracts into a white-hot core. Created when the charge starts, destroyed
// when it ends (the bolt itself is BulletRenderSystem's job).
class ChargingBulletRenderSystem : public ISystem
{
    const float CHARGING_BULLET_FRAMES = 5; // = CHARGE_SHOOT_FRAMES in InputSystem
    float time = 0.0f; // render-side clock for the shader ("uTime")

    // Puts the orb on the ship's nose and pushes the charge state to its shader.
    void UpdateOrb(Transform* orbTransform, MeshComponent* orbMesh,
        Transform* shipTransform, const SpaceShip* ship) const
    {
        // Same heading the bullet will be fired along (see InputServerSystem).
        const float yaw = glm::radians(shipTransform->getRotation().z);
        const glm::vec3 shipPos = shipTransform->getPosition();
        orbTransform->setPosition(glm::vec3(
            shipPos.x + std::cos(yaw) * SHIP_MUZZLE_OFFSET,
            shipPos.y + std::sin(yaw) * SHIP_MUZZLE_OFFSET,
            0.0f));

        // 0.2 on the first frame of the charge, 1.0 on the last.
        const float progress = std::clamp(
            (CHARGING_BULLET_FRAMES - ship->remainingShootFrames + 1) / CHARGING_BULLET_FRAMES,
            0.0f, 1.0f);

        if (!orbMesh->mesh) return;
        if (Material* mat = orbMesh->mesh->getMaterial())
        {
            mat->setFloat("uTime", time);
            mat->setFloat("uProgress", progress);
        }
    }

public:
    void Update(
        EntityManager& entityManager,
        std::vector<EventEntry>& events,
        bool isServer,
        float deltaTime
    ) override
    {
        time += deltaTime;

        auto playerQuery =
            entityManager.CreateQuery<Transform, Playable, SpaceShip>();

        auto effectQuery =
            entityManager.CreateQuery<Transform, ChargingShootEffect, MeshComponent>();

        for (auto [playerEntity, playerTransform, play, ship] : playerQuery)
        {
            bool foundIt = false;

            for (auto [effectEntity, effectTransform, effect, mesh] : effectQuery)
            {
                if (effect->entity == play->playerId)
                {
                    foundIt = true;

                    if (!ship->isShooting)
                    {
                        entityManager.DestroyEntity(effectEntity);
                    }
                    else
                    {
                        UpdateOrb(effectTransform, mesh, playerTransform, ship);
                    }
                }
            }

            if (!foundIt && ship->isShooting)
            {
                Entity effectEntity = entityManager.CreateEntity();

                Transform* effectTransform =
                    entityManager.AddComponent<Transform>(effectEntity, Transform{});

                // Constant size: the charge builds through the shader, not the scale.
                effectTransform->setScale(glm::vec3(CHARGE_ORB_RADIUS));

                ChargingShootEffect* effectComponent =
                    entityManager.AddComponent<ChargingShootEffect>(
                        effectEntity,
                        ChargingShootEffect{}
                    );

                effectComponent->entity = play->playerId;

                MeshComponent* orbMesh =
                    AddChargeOrbMesh(entityManager, effectEntity, play->playerId);

                UpdateOrb(effectTransform, orbMesh, playerTransform, ship);
            }
        }
    }
};

// Per-frame state of the laser bolts: the uniforms of laser_bolt.frag (a render-side
// clock, the bolt's age for the spawn flash and ramp-in, and how close it is to
// expiring, so it dissipates instead of vanishing) and the intensity of its point
// light, which follows the same age/fade. Purely visual — nothing here is synced or
// predicted. The bolt's position and heading are set when its entity is created.
class BulletRenderSystem : public ISystem
{
    const float FADE_TICKS = 6.0f; // the bolt dissipates over the last N ticks of its lifetime
    // The bolt is born right at the nose/hull edge, where a full-strength light
    // would blow it out for a frame or two, so the light swells in over this long instead.
    const float LIGHT_RAMP_SECONDS = 0.08f;
    float time = 0.0f;

public:
    void Update(
        EntityManager& entityManager,
        std::vector<EventEntry>& events,
        bool isServer,
        float deltaTime
    ) override
    {
        time += deltaTime;

        auto bulletQuery = entityManager.CreateQuery<ECSBullet, MeshComponent, BulletVisual>();
        for (auto [entity, bullet, mesh, visual] : bulletQuery)
        {
            visual->age += deltaTime;

            const float fade = std::clamp(bullet->lifetime / FADE_TICKS, 0.0f, 1.0f);

            if (PointLightComponent* light = entityManager.GetComponent<PointLightComponent>(entity))
            {
                const float ramp = std::clamp(visual->age / LIGHT_RAMP_SECONDS, 0.0f, 1.0f);
                light->intensity = LASER_BOLT_LIGHT_INTENSITY * ramp * ramp * (3.0f - 2.0f * ramp) * fade;
            }

            if (!mesh->mesh) continue;
            if (Material* mat = mesh->mesh->getMaterial())
            {
                mat->setFloat("uTime", time);
                mat->setFloat("uAge", visual->age);
                mat->setFloat("uFade", fade);
            }
        }
    }
};

class LinkAudioToBulletSystem : public ISystem
{
public:
	void Update(
		EntityManager& entityManager,
		std::vector<EventEntry>& events,
		bool isServer,
		float deltaTime
	) override
	{
		auto audioQuery = entityManager.CreateQuery<Transform, AudioSourceComponent, LinkAudioToBullet>();
		auto bulletQuery = entityManager.CreateQuery<Transform, ECSBullet>();
		for (auto [audioEntity, audioTransform, audio, link] : audioQuery)
		{
			for (auto [bulletEntity, bulletTransform, bullet] : bulletQuery)
			{
				if (bullet->id == link->bulletId)
				{
					audioTransform->setPosition(bulletTransform->getPosition());
					break;
				}
			}
		}
	}
};

class DestroyTimerSystem : public ISystem
{
public:
    void Update(
        EntityManager& entityManager,
        std::vector<EventEntry>& events,
        bool isServer,
        float deltaTime
    ) override
    {
        auto query = entityManager.CreateQuery<DestroyTimer>();

        for (auto [entity, destroyTimer] : query)
        {
            auto audio = entityManager.GetComponent<AudioSourceComponent>(entity);
            if (audio->pendingToDestroy) continue;
            destroyTimer->framesRemaining -= 1;

            if (destroyTimer->framesRemaining <= 0)
            {

                audio->pendingToDestroy = true;
            }
        }
    }
};


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

class UpdateListenerTransformSystem : public ISystem
{
public:
    void Update(
        EntityManager& entityManager,
        std::vector<EventEntry>& events,
        bool isServer,
        float deltaTime
    ) override
    {
        auto listenerQuery = entityManager.CreateQuery<Transform, AudioListenerComponent>();
        auto playerQuery = entityManager.CreateQuery<Transform, Playable, SpaceShip>();
        for (auto [listenerEntity, listenerTransform, listener] : listenerQuery)
        {
            for (auto [playerEntity, playerTransform, play, ship] : playerQuery)
            {
                if (play->isLocal)
                {
                    listenerTransform->setPosition(playerTransform->getPosition());
					listenerTransform->setRotation(playerTransform->getRotation());
                    break;
                }
            }
        }
    }
};

// Drives tile/wall/spoke/pillar visibility from the replicated state, and the
// per-wall beam animation (laser_wall.frag): each wall eases toward "solid",
// "warning" or "off" through a LaserWallVisual instead of popping in and out.
// Purely a render-side effect — nothing here is synced or predicted.
class LaserWallRenderSystem : public ISystem
{
    // Quick to energise so the wall still reads as a snap; slower to die so the
    // beam visibly collapses instead of vanishing.
    const float POWER_RISE_TIME = 0.20f;
    const float POWER_FALL_TIME = 0.30f;
    const float WARNING_BLEND_TIME = 0.15f;  // fade to/from the amber look
    const float WARNING_POWER = 0.5f;        // width/brightness of the amber preview beam
    const float WARNING_RAMP_TIME = 3.0f;    // = WARNING_THRESHOLD in LogicSystems.hpp; how long the stutter takes to go from mostly-off to mostly-on
    const float FLASH_DECAY_TIME = 0.35f;

    float time = 0.0f; // render-side clock for the beam shader ("uTime")

    static float MoveToward(float current, float target, float maxDelta)
    {
        if (current < target) return std::min(current + maxDelta, target);
        return std::max(current - maxDelta, target);
    }

    // Eases one wall/spoke toward what the replicated state says (solid /
    // warning / off), toggles its mesh, and pushes the result to the shader.
    void AnimateWall(MeshComponent* mesh, LaserWallVisual* vis, bool solid, bool warning, float deltaTime)
    {
        // Just energised: white-hot burst that decays.
        if (solid && !vis->wasSolid) vis->flash = 1.0f;
        vis->wasSolid = solid;
        vis->flash = std::max(0.0f, vis->flash - deltaTime / FLASH_DECAY_TIME);

        const float targetPower = solid ? 1.0f : (warning ? WARNING_POWER : 0.0f);
        const float riseFall = (targetPower > vis->power) ? POWER_RISE_TIME : POWER_FALL_TIME;
        vis->power = MoveToward(vis->power, targetPower, deltaTime / riseFall);

        vis->warning = MoveToward(vis->warning, warning ? 1.0f : 0.0f, deltaTime / WARNING_BLEND_TIME);
        vis->warnTime = warning ? vis->warnTime + deltaTime : 0.0f;

        // Fully faded out: stop drawing it altogether (also skips the uniform uploads).
        mesh->enabled = vis->power > 0.002f;
        if (!mesh->enabled || !mesh->mesh) return;

        if (Material* mat = mesh->mesh->getMaterial())
        {
            mat->setFloat("uTime", time);
            mat->setFloat("uIntensity", vis->power);
            mat->setFloat("uWarning", vis->warning);
            mat->setFloat("uWarnTension", std::min(vis->warnTime / WARNING_RAMP_TIME, 1.0f));
            mat->setFloat("uFlash", vis->flash);
        }
    }

public:
    void Update(
        EntityManager& entityManager,
        std::vector<EventEntry>& events,
        bool isServer,
        float deltaTime
    ) override
    {
        const int x_size = 5;
        const int y_size = 5;

        time += deltaTime;

        // Build active tile set
        std::unordered_set<int> activeTileIds;
        {
            auto tileActiveQuery = entityManager.CreateQuery<TileID>();
            for (auto [entity, tileId] : tileActiveQuery)
                if (tileId->active) activeTileIds.insert(tileId->id);
        }

        // Tiles: active flag drives visibility, warning drives fall
        {
            auto tileQuery = entityManager.CreateQuery<TileID, MeshComponent, Transform>();
            for (auto [entity, tileId, mesh, transform] : tileQuery)
            {
                mesh->enabled = tileId->active;

                if (tileId->active && tileId->warning)
                {
                    tileId->warningFallAccum += deltaTime;
                    float drop = tileId->warningFallAccum * tileId->warningFallAccum * 6.0f;
                    glm::vec3 pos = transform->getPosition();
                    transform->setPosition(glm::vec3(pos.x, pos.y, -4.0f - drop));
                }
                else if (tileId->active && !tileId->warning)
                {
                    tileId->warningFallAccum = 0.0f;
                    glm::vec3 pos = transform->getPosition();
                    transform->setPosition(glm::vec3(pos.x, pos.y, -4.0f));
                }
            }
        }

        // Walls and spokes
        {
            auto laserWallQuery = entityManager.CreateQuery<LaserWallID, MeshComponent, LaserWallVisual>();
            for (auto [entity, lwID, mesh, visual] : laserWallQuery)
            {
                bool isSpoke = entityManager.GetComponent<CenterSpoke>(entity) != nullptr;

                // solid = beam should be energised; warning = about to be
                // (disabled but flagged). Same rules as before, only the
                // result now feeds the animation instead of mesh->enabled.
                bool solid = false;
                bool warning = false;

                if (isSpoke)
                {
                    // Single-owner: no neighbour concept, only its own cell matters.
                    if (activeTileIds.count(lwID->cellId))
                    {
                        solid = lwID->enabled;
                        warning = lwID->warning && !lwID->enabled;
                    }
                }
                else
                {
                    // Shared edge: dedup means one entity per boundary, stored
                    // under whichever of its two cells happened to be visited
                    // first when the wall was built, so visibility has to be
                    // judged symmetrically — see ClassifyWallEdge.
                    WallEdgeState edgeState = ClassifyWallEdge(lwID->cellId, lwID->dir, activeTileIds);

                    if (edgeState == WallEdgeState::SoleBorder)
                    {
                        solid = true;
                    }
                    else if (edgeState == WallEdgeState::Interior)
                    {
                        solid = lwID->enabled;
                        warning = lwID->warning && !lwID->enabled;
                    }
                }

                AnimateWall(mesh, visual, solid, warning, deltaTime);
            }
        }

        // Pillars: hidden when ALL their cells are inactive
        {
            auto pillarQuery = entityManager.CreateQuery<PillarID, MeshComponent>();
            for (auto [entity, pid, mesh] : pillarQuery)
            {
                if (pid->cellsIsIn.empty()) { mesh->enabled = false; continue; }
                bool allGone = true;
                for (int cellId : pid->cellsIsIn)
                    if (activeTileIds.count(cellId)) { allGone = false; break; }
                mesh->enabled = !allGone;
            }
        }
    }
};

class ThrustersSoundSystem : public ISystem
{
public:
	void Update(
		EntityManager& entityManager,
		std::vector<EventEntry>& events,
		bool isServer,
		float deltaTime
	) override
	{
		auto audioQuery = entityManager.CreateQuery<Transform, AudioSourceComponent, ThrusterSound>();
		auto shipQuery = entityManager.CreateQuery<Transform, Playable, SpaceShip>();
		for (auto [audioEntity, soundT, audio, thrusterOwner] : audioQuery)
		{
			for (auto [shipEntity, shipT, play, ship] : shipQuery)
			{
				if (thrusterOwner->shipEntity != play->playerId) continue;
				if (!ship->isAlive)
				{
					// `play` only ever triggers alSourcePlay (see AudioSystem::Update) — it
					// never stops an already-looping source, so the engine loop would keep
					// looping forever at its last pitch/gain if we didn't silence it here.
					audio->play = false;
					audio->gain = 0.0f;
				}
				else
				{
					audio->play = true;

                    soundT->setPosition(shipT->getPosition());

					float speed = glm::length(glm::vec2(ship->velX, ship->velY));
                    float t = std::min(speed / 3.0f, 1.0f);

                    audio->gain = 0.05f + 0.45f * t;   // 0.05 idle → 0.50 full
                    audio->pitch = 0.5f + 0.7f * t;   // 0.5 idle → 1.2 full
				}
			}
		}
	}
};

// Drives the "uTime" uniform for every FluidSurface-tagged mesh (fluid.vert
// wave displacement, water.frag/lava.frag scrolling detail). Purely a
// render-side clock: no gameplay state, so it isn't synced or predicted —
// each client just animates on its own local timeline.
class FluidAnimationSystem : public ISystem
{
	float time = 0.0f;

public:
	void Update(
		EntityManager& entityManager,
		std::vector<EventEntry>& events,
		bool isServer,
		float deltaTime
	) override
	{
		time += deltaTime;

		auto query = entityManager.CreateQuery<MeshComponent, FluidSurface>();
		for (auto [entity, meshC, fluidTag] : query)
		{
			if (!meshC->mesh) continue;
			if (Material* mat = meshC->mesh->getMaterial())
				mat->setFloat("uTime", time);
		}
	}
};