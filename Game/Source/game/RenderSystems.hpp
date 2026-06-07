#pragma once
#include <unordered_set>
#include <algorithm>

#include "netcode/netcode_common.hpp"
#include "Utils/Input.hpp"
#include "ecs/ecs_common.hpp"
#include "ecs/UI/UIButton.hpp"
#include "ecs/UI/UIImage.hpp"
#include "ecs/UI/UIText.hpp"
#include "ecs/UI/UIElement.hpp"
#include "Components.hpp"
#include "GameState.hpp"
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

        // Check if the local player is alive
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

        // ── Game over: all players see the winner zoom regardless of alive state ──
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
                auto textQuery = entityManager.CreateQuery<UIElement, UIText>();
                for (auto [entity, element, text] : textQuery)
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

        // ── Normal gameplay ───────────────────────────────────────────────────
        if (localAlive)
        {
            targetPos = localPos;

            int aliveCount = 0;
            for (auto [e2, pt2, pl2, sh2] : playerQuery)
                if (sh2->isAlive) aliveCount++;

            auto textQuery = entityManager.CreateQuery<UIElement, UIText>();
            for (auto [entity, element, text] : textQuery)
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
                        // Find first alive player to watch (skip self)
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
                            // Collect alive player ids (excluding local)
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


                    for (auto effect : ParticlePresets::MakeExplosion()) 
                    {
						Entity e = entityManager.CreateEntity();
						Transform* t = entityManager.AddComponent<Transform>(e, Transform{});
						t->setPosition(playerTransform->getPosition());
						t->setScale(glm::vec3(2.0f, 2.0f,2.0f));
						entityManager.AddComponent<ParticleEmitterComponent>(e, effect);
						entityManager.AddComponent<ExplosionPlayerID>(e, ExplosionPlayerID{ play->playerId });
                    }


                    Entity audioEntity = entityManager.CreateEntity();
					Transform* audioTransform = entityManager.AddComponent<Transform>(audioEntity, Transform{});
					audioTransform->setPosition(playerTransform->getPosition());
                    AudioSourceComponent* audio = entityManager.AddComponent<AudioSourceComponent>(
                        audioEntity, AudioSourceComponent("explosion.wav", AudioChannel::SFX, false));
					audio->gain = 3.0f;
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

                auto textQuery = entityManager.CreateQuery<UIElement, UIText>();
                for (auto [uiEntity, element, text] : textQuery)
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

class ChargingBulletRenderSystem : public ISystem
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
            entityManager.CreateQuery<Transform, Playable, SpaceShip>();

        auto effectQuery =
            entityManager.CreateQuery<Transform, ChargingShootEffect, MeshComponent>();

        const float CHARGING_BULLET_FRAMES = 5;

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
                        effectTransform->setPosition(
                            glm::vec3(
                                playerTransform->getPosition().x,
                                playerTransform->getPosition().y,
                                0.0f
                            )
                        );

                        float scale = (CHARGING_BULLET_FRAMES - ship->remainingShootFrames + 1) * 1.5f;

                        effectTransform->setScale(
                            glm::vec3(scale, scale, 1.0f)
                        );
                    }
                }
            }

            if (!foundIt && ship->isShooting)
            {
                Entity effectEntity = entityManager.CreateEntity();

                Transform* effectTransform =
                    entityManager.AddComponent<Transform>(effectEntity, Transform{});

                effectTransform->setPosition(
                    glm::vec3(
                        playerTransform->getPosition().x,
                        playerTransform->getPosition().y,
                        0.0f
                    )
                );

                float scale =
                    (CHARGING_BULLET_FRAMES - ship->remainingShootFrames + 1) * 1.5f;

                effectTransform->setScale(
                    glm::vec3(scale, scale, 1.0f)
                );

                ChargingShootEffect* effectComponent =
                    entityManager.AddComponent<ChargingShootEffect>(
                        effectEntity,
                        ChargingShootEffect{}
                    );

                effectComponent->entity = play->playerId;

                auto shootingMat =
                    std::make_shared<Material>("generic.vert", "generic.frag");

                shootingMat->setVec3("uColor", glm::vec3(1.0f, 1.0f, 0.0f));

                entityManager.AddComponent<MeshComponent>(
                    effectEntity,
                    MeshComponent(new Mesh("charge.glb", shootingMat))
                );
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

        //Rotate ship

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

                // --- Position (same for both thruster and smoke) ---
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


                // --- Activation ---
                if (thrusterOwner->isSmoke)
                {
                    thrusterEmitter->enabled = !ship->isMovingForward;  // smoke when idle
                    thrusterTransform->setRotation(glm::vec3(
                        0.0f,
                        shipTransform->getRotation().z,
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

class LaserWallRenderSystem : public ISystem
{
    float warningTimer = 0.0f;
    const float WARNING_BLINK_INTERVAL = 0.25f;
    bool warningBlinkActive = false; // flips every WARNING_BLINK_INTERVAL, used for all warning walls/spokes

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

        // ── Advance warning blink timer ───────────────────────────────────────
        warningTimer += deltaTime;
        if (warningTimer >= WARNING_BLINK_INTERVAL)
        {
            warningTimer -= WARNING_BLINK_INTERVAL;
            warningBlinkActive = !warningBlinkActive;
        }

        // ── Build active tile set ─────────────────────────────────────────────
        std::unordered_set<int> activeTileIds;
        {
            auto tileActiveQuery = entityManager.CreateQuery<TileID>();
            for (auto [entity, tileId] : tileActiveQuery)
                if (tileId->active) activeTileIds.insert(tileId->id);
        }

        // ── Tiles: active flag drives visibility, warning drives fall ─────────
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

        // ── Walls and spokes ──────────────────────────────────────────────────
        {
            auto laserWallQuery = entityManager.CreateQuery<LaserWallID, MeshComponent>();
            for (auto [entity, lwID, mesh] : laserWallQuery)
            {
                bool isSpoke = entityManager.GetComponent<CenterSpoke>(entity) != nullptr;
                bool ownerActive = activeTileIds.count(lwID->cellId) > 0;

                if (!ownerActive)
                {
                    mesh->enabled = false;
                    continue;
                }

                if (isSpoke)
                {
                    if (lwID->warning && !lwID->enabled)
                        mesh->enabled = warningBlinkActive;
                    else
                        mesh->enabled = lwID->enabled;
                    continue;
                }

                // Non-spoke walls
                int cx = lwID->cellId / y_size;
                int cy = lwID->cellId % y_size;
                int nx = cx, ny = cy;
                switch (lwID->dir)
                {
                case CellCardinalDirection::Left:  nx--; break;
                case CellCardinalDirection::Right: nx++; break;
                case CellCardinalDirection::Down:  ny--; break;
                case CellCardinalDirection::Up:    ny++; break;
                default: break;
                }

                bool neighbourInactive = nx < 0 || nx >= x_size || ny < 0 || ny >= y_size
                    || !activeTileIds.count(nx * y_size + ny);

                if (neighbourInactive)
                {
                    mesh->enabled = true;
                }
                else if (lwID->warning && !lwID->enabled)
                {
                    mesh->enabled = warningBlinkActive;
                }
                else
                {
                    mesh->enabled = lwID->enabled;
                }
            }
        }

        // ── Pillars: hidden when ALL their cells are inactive ─────────────────
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
					audio->play = false;
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