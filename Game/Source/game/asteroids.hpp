#ifndef ASTEROIDS
#define ASTEROIDS

// Client side of the game: AsteroidShooterClientGame (the shared logic plus local input and the exit-to-lobby check)
// and the renderer. The simulation itself is in AsteroidsLogic.hpp, which GameServer includes on its own.

#include "AsteroidsLogic.hpp"

#include "OpenGL/OpenGLIncludes.hpp"
#include "Utils/Input.hpp"
#include "GameActions.hpp"
#include "OpenGL/IGameRenderer.hpp"
#include "OpenGL/IECSGameRenderer.hpp"
#include "OpenGL/MeshComponent.hpp"
#include "ecs/UI/UIButton.hpp"
#include "ecs/UI/UIImage.hpp"
#include "ecs/UI/UIText.hpp"
#include "ecs/UI/UIElement.hpp"

#include "LogicSystems/ExitCheckerSystem.hpp"
#include "RenderSystems/LaserVisuals.hpp"
#include "RenderSystems/CameraFollowSystem.hpp"
#include "RenderSystems/OnDeathRenderSystem.hpp"
#include "RenderSystems/MatchStartCountdownRenderSystem.hpp"
#include "RenderSystems/ChargingBulletRenderSystem.hpp"
#include "RenderSystems/BulletRenderSystem.hpp"
#include "RenderSystems/LinkThrusterToShipSystem.hpp"
#include "RenderSystems/LaserWallRenderSystem.hpp"
#include "RenderSystems/ReflectionProbeRefreshSystem.hpp"
#include "RenderSystems/UpdateListenerTransformSystem.hpp"
#include "RenderSystems/ThrustersSoundSystem.hpp"
#include "RenderSystems/FluidAnimationSystem.hpp"
#include "RenderSystems/DestroyTimerSystem.hpp"
#include "RenderSystems/ShieldRenderSystem.hpp"
#include "RenderSystems/AbilityHudSystem.hpp"
#include "RenderSystems/PlayerNameTagSystem.hpp"
#include "Explosion.hpp"
#include "PauseMenu.hpp"

#include "OpenAL/AudioManager.hpp"
#include "OpenAL/AudioComponents.hpp"

#include "NetTFG_Engine.hpp"




// The game logic as the client runs it (prediction / offline): the shared simulation plus reading the local player's
// input and the exit-to-lobby check, which reach InputMap and NetTFG_Engine and so can't be in the headless logic.
class AsteroidShooterClientGame : public AsteroidShooterGame {
public:
    std::unique_ptr<IGameLogic> Clone() const override {
        return std::make_unique<AsteroidShooterClientGame>();
    }

    // Game tick thread: reads InputMap's snapshot (keyboard or gamepad, whatever is bound), never Input directly.
    InputBlob GenerateLocalInput() override {
        const InputMap::TickInput in = InputMap::Get().ConsumeTickInput();
        InputBlob buf = MakeZeroInputBlob();
        uint8_t m = INPUT_NONE;
        // Turning and thrust also carry how far the stick / trigger is pushed (see InputMask.hpp); keys send full.
        if (in.Down(GameAction::TurnLeft)) {
            m |= INPUT_LEFT;
            buf.data[INPUT_BYTE_LEFT] = EncodeInputIntensity(in.Value(GameAction::TurnLeft));
        }
        if (in.Down(GameAction::TurnRight)) {
            m |= INPUT_RIGHT;
            buf.data[INPUT_BYTE_RIGHT] = EncodeInputIntensity(in.Value(GameAction::TurnRight));
        }
        if (in.Down(GameAction::Thrust)) {
            m |= INPUT_TOP;
            buf.data[INPUT_BYTE_THRUST] = EncodeInputIntensity(in.Value(GameAction::Thrust));
        }
        if (in.Down(GameAction::Shoot)) m |= INPUT_SHOOT;
        if (in.Down(GameAction::Dash)) m |= INPUT_DASH;
        if (in.Down(GameAction::Shield)) m |= INPUT_SHIELD;
        buf.data[0] = m;
        return buf;
    }

    void InitECSLogic(GameStateBlob& state) override {
        AsteroidShooterGame::InitECSLogic(state);
        // Last, as before the split: after the shared systems.
        world.AddSystem(std::make_unique<ExitCheckerSystem>());
    }
};


class AsteroidShooterGameRenderer : public IECSGameRenderer {
public:

    void printGameState(const AsteroidShooterGameState& state) const
    {
        Debug::Info("GameState") << "=== Asteroid Shooter Game State ===\n";
        for (int i = 0; i < 2; i++) {
            Debug::Info("GameState") << "  Player " << i << ":\n";
            Debug::Info("GameState") << "    Position: (" << state.posX[i] << ", " << state.posY[i] << ")\n";
            Debug::Info("GameState") << "    Rotation: " << state.rot[i] << " degrees\n";
            Debug::Info("GameState") << "    Health: " << state.health[i] << "\n";
            Debug::Info("GameState") << "    Alive: " << (state.alive[i] ? "true" : "false") << "\n";
            Debug::Info("GameState") << "    Spectating: " << (!state.alive[i] ? "true" : "false") << "\n";
            Debug::Info("GameState") << "    Remaining shoot frames: " << state.remaingShootFrames[i] << "\n";
            Debug::Info("GameState") << "    Shoot Cooldown: " << state.shootCooldown[i] << "\n";
        }
        Debug::Info("GameState") << "  Active Bullet Count: " << state.bulletCount << "\n";
        Debug::Info("GameState") << "===================================\n";
    }

    void GameState_To_ECSWorld(const GameStateBlob& state) override
    {
        AsteroidShooterGameState s =
            *reinterpret_cast<const AsteroidShooterGameState*>(state.data);

        EntityManager& em = world.GetEntityManager();

        // Players
        auto playerQuery = em.CreateQuery<Transform, Playable, SpaceShip>();
        for (auto [entity, transform, play, ship] : playerQuery)
        {
            int p = play->playerId;
            transform->setPosition(glm::vec3(s.posX[p], s.posY[p], 0.0f));
            transform->setRotation(glm::vec3(0.0f, 0.0f, s.rot[p]));
            ship->health = s.health[p];
            ship->isAlive = s.alive[p];
            ship->shipZRotation = static_cast<int>(s.rot[p]);
            ship->isShooting = s.isShooting[p];
            ship->isMovingForward = s.isMovingForward[p];
            ship->shipInclination = s.shipInclination[p];
            ship->remainingShootFrames = s.remaingShootFrames[p];
            ship->shootCooldown = s.shootCooldown[p];
            ship->velX = s.velX[p];
            ship->velY = s.velY[p];
            ship->angularVel = s.angularVel[p];
            ship->dashTicks = s.dashTicks[p];
            ship->dashCooldown = s.dashCooldown[p];
            ship->shieldTicks = s.shieldTicks[p];
            ship->shieldCooldown = s.shieldCooldown[p];
        }

        // Bullets — collect ECS ids
        std::set<int> ecsActiveBulletIds;
        {
            auto q = em.CreateQuery<ECSBullet>();
            for (auto [e, ecsb] : q)
                ecsActiveBulletIds.insert(ecsb->id);
        }

        // Collect state ids
        std::set<int> stateActiveBulletIds;
        for (int i = 0; i < MAX_BULLETS; i++)
            if (s.bullets[i].active)
                stateActiveBulletIds.insert(s.bullets[i].id);

        // Remove stale bullets
        {
            auto q = em.CreateQuery<ECSBullet, MeshComponent>();
            for (auto [e, ecsb, mesh] : q)
                if (stateActiveBulletIds.find(ecsb->id) == stateActiveBulletIds.end())
                    em.DestroyEntity(e);
        }

        // Update or create bullets
        for (int i = 0; i < MAX_BULLETS; i++)
        {
            const Bullet& b = s.bullets[i];
            if (!b.active) continue;

            bool found = false;
            auto q = em.CreateQuery<Transform, ECSBullet, MeshComponent>();
            for (auto [e, transform, ecsb, mesh] : q)
            {
                if (ecsb->id == b.id)
                {
                    transform->setPosition(glm::vec3(b.posX, b.posY, 0.0f));
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
                Entity newBullet = em.CreateEntity();
                Transform* t = em.AddComponent<Transform>(newBullet, Transform{});
                t->setPosition(glm::vec3(b.posX, b.posY, 0.0f));
                // The bolt is stretched along its local +X: face the direction of flight
                // (constant for the bullet's whole life, so it's set once, here).
                t->setRotation(glm::vec3(0.0f, 0.0f, glm::degrees(std::atan2(b.velY, b.velX))));
                t->setScale(LASER_BOLT_SCALE);

                em.AddComponent<ECSBullet>(newBullet, ECSBullet{ b.id, b.velX, b.velY, b.ownerId, b.lifetime });
                em.AddComponent<BulletVisual>(newBullet, BulletVisual{});
                AddLaserBoltMesh(em, newBullet, b.id);
                AddLaserBoltLight(em, newBullet);

                // The shot sounds where it was fired: at the shooter's ship. The bullet is only first seen here a
                // round trip after it left the muzzle (for remote shots, plus their latency), already 150 u/s further on.
                glm::vec3 shotPos(b.posX, b.posY, 0.0f);
                if (b.ownerId >= 0 && b.ownerId < NUM_PLAYERS)
                    shotPos = glm::vec3(s.posX[b.ownerId], s.posY[b.ownerId], 0.0f);

                Entity bulletSound = em.CreateEntity();
                Transform* st = em.AddComponent<Transform>(bulletSound, Transform{});
                st->setPosition(shotPos);
                DestroyTimer* dt = em.AddComponent<DestroyTimer>(bulletSound, DestroyTimer{});
                dt->secondsRemaining = 3.0f;
                AudioSourceComponent* audio = em.AddComponent<AudioSourceComponent>(
                    bulletSound, AudioSourceComponent("shoot.wav", AudioChannel::SFX, false));
                audio->play = true;
				audio->gain = 1.0f;
				ApplyArenaAttenuation(*audio);
            }
        }

        const int x_size = 5;
        const int y_size = 5;

        // Sync tile active + warning flags
        {
            auto tileQuery = em.CreateQuery<TileID>();
            for (auto [entity, tileId] : tileQuery)
            {
                int cx = tileId->id / y_size;
                int cy = tileId->id % y_size;
                tileId->active = s.tilesActive[cx][cy];
                tileId->warning = s.tilesWarning[cx][cy];
            }
        }

        // Sync wall enabled + warning state
        {
            auto wallQuery = em.CreateQuery<LaserWallID>();
            for (auto [entity, lwid] : wallQuery)
            {
                if (em.GetComponent<CenterSpoke>(entity) != nullptr) continue;
                lwid->enabled = EdgeWallSlot(s.hWalls, s.vWalls, lwid->cellId, lwid->dir, lwid->half);
                lwid->warnCode = EdgeWallSlot(s.hWallsWarning, s.vWallsWarning, lwid->cellId, lwid->dir, lwid->half);
                lwid->warning = lwid->warnCode != 0;
            }

            auto spokeQuery = em.CreateQuery<LaserWallID, CenterSpoke>();
            for (auto [entity, lwid, spoke] : spokeQuery)
            {
                int cx = lwid->cellId / y_size;
                int cy = lwid->cellId % y_size;
                int dirIdx = SpokeIndex(lwid->dir);
                lwid->enabled = s.cWalls[cx][cy][dirIdx];
                lwid->warnCode = s.cWallsWarning[cx][cy][dirIdx];
                lwid->warning = lwid->warnCode != 0;
            }
        }

        // Match-start countdown
        {
            auto timerQuery = em.CreateQuery<MatchStartTimer>();
            for (auto [entity, timer] : timerQuery)
                timer->ticksRemaining = s.startCountdownTicks;
        }
    }

    void InitECSRenderer(const GameStateBlob& state, OpenGLWindow* window) override {
        this->window = window;

        AsteroidShooterGameState s;
        std::memcpy(&s, state.data, sizeof(AsteroidShooterGameState));

        world.GetEntityManager().RegisterComponentType<SpaceShip>();
        world.GetEntityManager().RegisterComponentType<ECSBullet>();
        world.GetEntityManager().RegisterComponentType<ChargingShootEffect>();
        world.GetEntityManager().RegisterComponentType<DestroyTimer>();
        world.GetEntityManager().RegisterComponentType<TileID>();
        world.GetEntityManager().RegisterComponentType<PillarID>();
        world.GetEntityManager().RegisterComponentType<LaserWallID>();
        world.GetEntityManager().RegisterComponentType<CenterSpoke>();
        world.GetEntityManager().RegisterComponentType<ThrusterOwner>();
        world.GetEntityManager().RegisterComponentType<SpectatorState>();
		world.GetEntityManager().RegisterComponentType<JustDeathChecker>();
		RegisterExplosionComponents(world.GetEntityManager());
		PreloadExplosionAssets(world.GetEntityManager());
		world.GetEntityManager().RegisterComponentType<ExitButtonChecker>();
		world.GetEntityManager().RegisterComponentType<ThrusterSound>();
		world.GetEntityManager().RegisterComponentType<FluidSurface>();
		world.GetEntityManager().RegisterComponentType<LaserWallVisual>();
		world.GetEntityManager().RegisterComponentType<BulletVisual>();
		world.GetEntityManager().RegisterComponentType<GameStatusText>();
		world.GetEntityManager().RegisterComponentType<ShieldEffect>();
		world.GetEntityManager().RegisterComponentType<AbilityHudIcon>();
		RegisterPauseMenuComponents(world.GetEntityManager());
		world.GetEntityManager().RegisterComponentType<MatchStartTimer>();

		Entity matchStartEntity = world.GetEntityManager().CreateEntity();
        world.GetEntityManager().AddComponent<MatchStartTimer>(matchStartEntity, MatchStartTimer{ MATCH_START_COUNTDOWN_TICKS });

        // Sun
        Entity sunEntity = world.GetEntityManager().CreateEntity();
        world.GetEntityManager().AddComponent<Transform>(sunEntity, Transform{});
        DirectionalLightComponent* sunLight =
            world.GetEntityManager().AddComponent<DirectionalLightComponent>(sunEntity, DirectionalLightComponent{});
        sunLight->color = glm::vec3(1.0f, 0.95f, 0.8f);
        // Where the sky panorama's sun is (after its rotation below): azimuth 73 deg, 16 deg above the horizon. Low, so
        // shadows run long across the arena; matching it keeps reflections of the sun on the same spot as the light's
        // highlights. Points from the sun toward the scene.
        sunLight->direction = glm::normalize(glm::vec3(-0.2760f, -0.9198f, -0.2790f));

        // Sky for the environment lighting: what the ships' and pillars' metal reflects wherever no probe sees the arena
        // (mostly straight up, the camera looks down), and the ambient light. planeta_lava_4k.hdr (listed in
        // online_level.ntfg) is an equirectangular panorama of a lava plain under a dark sky, authored Y-up like any
        // HDRI; the arena is Z-up, hence upAxis. Its sun sits at azimuth ~141 deg / elevation ~16 deg with no rotation;
        // -68 deg turns it to azimuth 73 deg, where the directional light above was aimed (same elevation), so the sun
        // seen in reflections and the light's highlights and shadows coincide.
        EnvironmentSkyComponent* sky =
            world.GetEntityManager().AddComponent<EnvironmentSkyComponent>(sunEntity, EnvironmentSkyComponent{});
        sky->setEquirect("planeta_lava_4k.hdr");
        sky->upAxis = glm::vec3(0.0f, 0.0f, 1.0f);
        sky->rotationDegrees = -68.0f;
        sky->intensity = 1.0f;

        // Players
        for (int i = 0; i < NUM_PLAYERS; ++i)
        {

            Entity player = world.GetEntityManager().CreateEntity();
            Transform* t = world.GetEntityManager().AddComponent<Transform>(player, Transform{});
            t->setPosition(glm::vec3(s.posX[i], s.posY[i], 0.0f));
            t->setRotation(glm::vec3(0.0f, 0.0f, s.rot[i]));
            t->setScale(glm::vec3(5.0f, 5.0f, 5.0f));

            world.GetEntityManager().AddComponent<Playable>(
                player, Playable{ i, MakeZeroInputBlob(), (i == playerId) });
            world.GetEntityManager().AddComponent<SpaceShip>(player, SpaceShip{ 1, -1, 0, true });
            MeshComponent* shipMesh = world.GetEntityManager().AddComponent<MeshComponent>(
                player, MeshComponent(new Mesh("ship_low.glb")));
            shipMesh->reflectionProbeVisible = false;   // moves: would stay frozen in the probe captures

			world.GetEntityManager().AddComponent<JustDeathChecker>(player, JustDeathChecker{});

            // Player name over the ship, filled and shown/hidden by PlayerNameTagSystem. Billboard above the ship on
            // screen, whatever its heading; ~1 world unit is ~30 px at the follow camera's distance.
            WorldText* nameTag = world.GetEntityManager().AddComponent<WorldText>(player, WorldText{ "", 0.8f });
            nameTag->localOffset = glm::vec2(0.0f, 3.5f);
            nameTag->SetColor(1.0f, 1.0f, 1.0f, 0.9f);
            nameTag->visible = false;

            if (i == playerId)
            {
				Entity listenerForShipEntity = world.GetEntityManager().CreateEntity();
                Transform* tL = world.GetEntityManager().AddComponent<Transform>(listenerForShipEntity, Transform{});
                tL->setPosition(glm::vec3(s.posX[i], s.posY[i], 0.0f));
                world.GetEntityManager().AddComponent<AudioListenerComponent>(listenerForShipEntity, AudioListenerComponent{});
            }

			Entity thrusterSoundEntity = world.GetEntityManager().CreateEntity();
			Transform* tS = world.GetEntityManager().AddComponent<Transform>(thrusterSoundEntity, Transform{});
			tS->setPosition(glm::vec3(s.posX[i], s.posY[i], 0.0f));
			AudioSourceComponent* audio = world.GetEntityManager().AddComponent<AudioSourceComponent>(
				thrusterSoundEntity, AudioSourceComponent("engine.wav", AudioChannel::SFX, true));
			audio->gain = 0.0f; // Start muted, will be unmuted when moving
			audio->play = true;
			audio->loop = true;
			ApplyArenaAttenuation(*audio);
			world.GetEntityManager().AddComponent<ThrusterSound>(thrusterSoundEntity, ThrusterSound{ i });

            // Thrusters for this player
            struct ThrusterDef { bool isSmoke; bool isLeft; };
            constexpr ThrusterDef thrusterDefs[] = {
                { false, false },  // right thruster
                { false, true  },  // left thruster
                { true,  true  },  // left smoke
                { true,  false },  // right smoke
            };

            for (const auto& def : thrusterDefs)
            {
                Entity thrusterEntity = world.GetEntityManager().CreateEntity();
                Transform* tt = world.GetEntityManager().AddComponent<Transform>(thrusterEntity, Transform{});
                tt->setPosition(glm::vec3(0.0f, 0.0f, 2.0f));
                tt->setRotation(glm::vec3(0.0f, 90.0f, 0.0f));
                tt->setScale(glm::vec3(8.0f, 8.0f, 8.0f));

                world.GetEntityManager().AddComponent<ThrusterOwner>(
                    thrusterEntity, ThrusterOwner{ i, def.isSmoke, def.isLeft });

                if (!def.isSmoke)
                {
                    auto* particle = world.GetEntityManager().AddComponent<ParticleEmitterComponent>(
                        thrusterEntity, ParticlePresets::SpaceshipThruster());
                    particle->emissionRate = 300.0f;
                    particle->startLifetime = 0.05f;
                }
                else
                {
                    auto* particle = world.GetEntityManager().AddComponent<ParticleEmitterComponent>(
                        thrusterEntity, ParticlePresets::Smoke());
                    particle->emissionRate = 6.0f;
                    particle->startLifetime = 2.5f;
                    particle->startSpeed = 0.8f;
                    particle->startSize = 0.15f;
                    particle->endSize = 0.5f;
                    particle->gravityModifier = 0.15f;
                    particle->startColor = glm::vec4(0.08f, 0.08f, 0.08f, 0.8f);
                    particle->endColor = glm::vec4(0.05f, 0.05f, 0.05f, 0.0f);
                    particle->shape = EmitterShape::Cone;
                    particle->shapeConeAngle = 0.2f;
                    particle->lifetimeVariance = 0.6f;
                    particle->turbulenceStrength = 0.1f;
                    particle->enabled = false;
                }
            }
        }

        // Camera
        Entity camera = world.GetEntityManager().CreateEntity();
        Transform* camTrans = world.GetEntityManager().AddComponent<Transform>(camera, Transform{});
        camTrans->setPosition(glm::vec3(0.0f, 0.0f, 18.0f));
        Camera* camSettings = world.GetEntityManager().AddComponent<Camera>(camera, Camera{});
        // Near plane matters: depth is 32F non-reversed, so at 0.001 the z-buffer resolves ~0.1 units at 40 away and ~1 at 150
        // (lasers z-fighting pillars). The camera never gets within several units of anything, so 0.5 is safe and ~500x more precise.
        camSettings->setPerspective(45.0f, window->getAspectRatio(), 0.5f, 1000.0f);
        camSettings->setTarget(glm::vec3(0.0f, 0.0f, 0.0f));
        camSettings->setUp(glm::vec3(0.0f, 1.0f, 0.0f));

        // Health UI
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
        text->SetColor(1.0f, 1.0f, 1.0f, 1.0f);
        text->SetFont("default");
        // Tags this as the game's own status label, distinct from the
        // DebugOverlay's UIText, so the render systems' UIText queries
        // (REMAINING/YOU DIED/WINS) don't also reposition the debug HUD.
        world.GetEntityManager().AddComponent<GameStatusText>(healthText, GameStatusText{});

        // Ability icons (propulsion / laser shield) with their cooldowns, bottom centre.
        AbilityHud::Build(world.GetEntityManager());

		// Exit flag, synced to the logic side below; the pause menu is the only way to set it.
		Entity exitCheckerEntity = world.GetEntityManager().CreateEntity();
		ExitButtonChecker* exitChecker = world.GetEntityManager().AddComponent<ExitButtonChecker>(exitCheckerEntity, ExitButtonChecker{});

		// Escape menu: its "Sí" is the way back to the main menu.
		BuildPauseMenu(world.GetEntityManager(), exitChecker);

        renderDataTransferToLogicCallback = [](IECSGameLogic* logic, IECSGameRenderer* renderer) {
                if (!logic) {
                    return;
                }

                AsteroidShooterGame* gameLogic = dynamic_cast<AsteroidShooterGame*>(logic);
                AsteroidShooterGameRenderer* gameRenderer = dynamic_cast<AsteroidShooterGameRenderer*>(renderer);

                if (!gameLogic || !gameRenderer) {

					if (!gameLogic) {
						Debug::Error("Asteroids") << "Logic pointer is null in renderDataTransferToLogicCallback.\n";
					}
					if (!gameRenderer) {
						Debug::Error("Asteroids") << "Renderer pointer is null in renderDataTransferToLogicCallback.\n";
					}

                    return;
                }

				auto& entityManagerLogic = gameLogic->world.GetEntityManager();
				auto& entityManagerRenderer = gameRenderer->world.GetEntityManager();

                // Sync ExitButtonChecker value
				auto exitButtonQueryLogic = entityManagerLogic.CreateQuery<ExitButtonChecker>();
				auto exitButtonQueryRenderer = entityManagerRenderer.CreateQuery<ExitButtonChecker>();

				for (auto [entityLogic, exitCheckerLogic] : exitButtonQueryLogic) {
					for (auto [entityRenderer, exitCheckerRenderer] : exitButtonQueryRenderer) {
                        exitCheckerLogic->exitPressed = exitCheckerRenderer->exitPressed;
					}
				}
            };
		

        // Lava floor
        Entity lavaFloor = world.GetEntityManager().CreateEntity();
        Transform* lavaTrans = world.GetEntityManager().AddComponent<Transform>(lavaFloor, Transform{});
        lavaTrans->setPosition(glm::vec3(-20.0f, -20.0f, -12.0f));
        lavaTrans->setRotation(glm::vec3(90.0f, 0.0f, 0.0f));
        lavaTrans->setScale(glm::vec3(5.0f, 5.0f, 5.0f));

        auto lavaMat = std::make_shared<Material>("fluid.vert", "lava.frag");
        // Molten swell: alive but slower/heavier than water. Amplitude is in local units (floor scaled x5): 0.14 = ~2.9 world
        // units peak to peak. It was 0.45 when fluid.vert displaced mostly along the plane; with the axes fixed that'd be ~9.4.
        lavaMat->setFloat("uWaveAmplitude", 0.14f);
        lavaMat->setFloat("uWaveSpeed", 0.7f);
        lavaMat->setFloat("uWaveScale", 4.0f);
        lavaMat->setVec3("uRockColor", glm::vec3(0.05f, 0.03f, 0.03f));
        lavaMat->setVec3("uCrackColor", glm::vec3(0.9f, 0.25f, 0.05f));
        lavaMat->setVec3("uHotColor", glm::vec3(3.0f, 1.1f, 0.15f));
        lavaMat->setFloat("uEmissiveStrength", 2.5f);

        MeshComponent* lavaMesh = world.GetEntityManager().AddComponent<MeshComponent>(
            lavaFloor, MeshComponent(new Mesh("lava.glb", lavaMat)));
        lavaMesh->castShadows = false;
        world.GetEntityManager().AddComponent<FluidSurface>(lavaFloor, FluidSurface{});

        auto& em = world.GetEntityManager();

        // Arena geometry comes from ArenaLayout, shared with InitECSLogic: every beam matches its logic entity one to one
        // (same cellId, dir, half), so each reads exactly its own state slot (EdgeWallSlot).
        const ArenaLayout::Layout arena = ArenaLayout::Build();

        // Tiles
        for (const auto& tile : arena.tiles)
        {
            Entity e = em.CreateEntity();
            Transform* t = em.AddComponent<Transform>(e, Transform{});
            t->setPosition(tile.pos);
            t->setScale(glm::vec3(1.0f));
            em.AddComponent<TileID>(e, TileID{ tile.cellId });
            em.AddComponent<MeshComponent>(e, MeshComponent(new Mesh("tile.glb")));
        }

        // Reflection probes: one per cell, at ship height over its centre. Influence = the cell plus an 8-unit overlap
        // into the neighbours (they cross-fade there) and deep enough below to cover the tiles and the lava; parallax
        // proxy = the cell itself from the tile surface up, where the pillars and walls actually stand. Re-captured
        // around a tile when it drops out (ReflectionProbeRefreshSystem).
        {
            constexpr float kCellHalf = ArenaLayout::CELL_SIZE / 2.0f;
            constexpr float kBlend = 8.0f;
            for (int cx = 0; cx < MAP_SIZE; ++cx)
                for (int cy = 0; cy < MAP_SIZE; ++cy)
                {
                    const glm::vec2 c = ArenaLayout::CellCentre(cx, cy);
                    Entity e = em.CreateEntity();
                    Transform* t = em.AddComponent<Transform>(e, Transform{});
                    t->setPosition(glm::vec3(c, 2.0f));
                    ReflectionProbeComponent* probe = em.AddComponent<ReflectionProbeComponent>(e, ReflectionProbeComponent{});
                    probe->boxMin = glm::vec3(c - glm::vec2(kCellHalf + kBlend), -20.0f);
                    probe->boxMax = glm::vec3(c + glm::vec2(kCellHalf + kBlend), 40.0f);
                    probe->blendDistance = kBlend;
                    probe->parallaxBoxMin = glm::vec3(c - glm::vec2(kCellHalf), -4.0f);
                    probe->parallaxBoxMax = glm::vec3(c + glm::vec2(kCellHalf), 30.0f);
                    probe->captureNear = 0.5f;
                    probe->captureFar = 1000.0f;
                }
        }

        // Pillars
        for (const auto& pillar : arena.pillars)
        {
            Entity e = em.CreateEntity();
            Transform* t = em.AddComponent<Transform>(e, Transform{});
            t->setPosition(pillar.pos);
            t->setRotation(glm::vec3(90.0f, 0.0f, 0.0f));
            t->setScale(glm::vec3(1.0f));
            em.AddComponent<PillarID>(e, pillar.id);
            em.AddComponent<MeshComponent>(e, MeshComponent(new Mesh("pilar.glb")));
        }

        // Beam mesh at its design scale: local Z spans +-20 world units so each end lands on a pillar axis (40 apart); radius 2,
        // tapering over the last 8 units to 0.9 so the ends hide inside the pillar (radius ~1.3). The taper is baked into
        // laser_beam.glb at this scale. Only the visual entities use it; logic-side wall entities keep theirs.
        const char* const laserBeamMesh = "laser_beam.glb";
        const glm::vec3   laserBeamScale(2.0f, 2.0f, 20.0f);

        // One Material per wall/spoke (own power/warning/flash uniforms, driven by LaserWallRenderSystem); the program is
        // shared via ShaderLoader's cache. seed desyncs the noise between neighbours. Drawn additively, so the colours
        // below are light ADDED to the scene, not a surface colour.
        auto makeLaserWallMaterial = [](const glm::vec3& pos)
            {
                auto mat = std::make_shared<Material>("laser_wall.vert", "laser_wall.frag");
                mat->setVec3("uBeamColor", glm::vec3(1.0f, 0.07f, 0.04f));
                mat->setVec3("uWarnColor", glm::vec3(1.0f, 0.5f, 0.04f));
                mat->setVec3("uHotColor", glm::vec3(1.0f, 0.7f, 0.55f));
                mat->setFloat("uGlowStrength", 1.6f);
                mat->setFloat("uCoreStrength", 3.0f);
                mat->setFloat("uSeed", pos.x * 0.173f + pos.y * 0.291f);
                return mat;
            };

        for (const auto& w : arena.walls)
        {
            Entity e = em.CreateEntity();
            Transform* t = em.AddComponent<Transform>(e, Transform{});
            t->setPosition(w.pos);
            t->setRotation(w.rot);
            t->setScale(laserBeamScale);
            LaserWallID lwid(ArenaLayout::CellId(w.cellX, w.cellY), w.dir, w.half);
            lwid.enabled = w.onBorder;
            em.AddComponent<LaserWallID>(e, lwid);
            em.AddComponent<LaserWallVisual>(e, LaserWallVisual{});
            MeshComponent* mc = em.AddComponent<MeshComponent>(e,
                MeshComponent(new Mesh(laserBeamMesh, makeLaserWallMaterial(w.pos))));
            mc->castShadows = false;
            mc->additive = true;
        }

        // Center spokes
        for (const auto& s : arena.spokes)
        {
            Entity e = em.CreateEntity();
            Transform* t = em.AddComponent<Transform>(e, Transform{});
            t->setPosition(s.pos);
            t->setRotation(s.rot);
            t->setScale(laserBeamScale);
            LaserWallID lwid(s.cellId, s.dir);
            lwid.enabled = false;
            em.AddComponent<LaserWallID>(e, lwid);
            em.AddComponent<CenterSpoke>(e, CenterSpoke{});
            em.AddComponent<LaserWallVisual>(e, LaserWallVisual{});
            MeshComponent* mc = em.AddComponent<MeshComponent>(e,
                MeshComponent(new Mesh(laserBeamMesh, makeLaserWallMaterial(s.pos))));
            mc->castShadows = false;
            mc->additive = true;
        }

        // Render systems
        world.AddSystem(std::make_unique<CameraFollowSystem>());
        world.AddSystem(std::make_unique<OnDeathRenderSystem>());
        world.AddSystem(std::make_unique<ExplosionEffectsSystem>());
        // After both of the above: overrides GameStatusText with the
        // countdown while the pre-match freeze is active, otherwise a no-op.
        world.AddSystem(std::make_unique<MatchStartCountdownRenderSystem>());
        world.AddSystem(std::make_unique<ChargingBulletRenderSystem>());
        world.AddSystem(std::make_unique<BulletRenderSystem>());
        world.AddSystem(std::make_unique<ShieldRenderSystem>());
        world.AddSystem(std::make_unique<LinkThrusterToShipSystem>());
        world.AddSystem(std::make_unique<LaserWallRenderSystem>());
        world.AddSystem(std::make_unique<ReflectionProbeRefreshSystem>());   // after the tiles' visibility is updated
		world.AddSystem(std::make_unique<UpdateListenerTransformSystem>());
		world.AddSystem(std::make_unique<ThrustersSoundSystem>());
        world.AddSystem(std::make_unique<FluidAnimationSystem>());
        world.AddSystem(std::make_unique<DestroyTimerSystem>());
        world.AddSystem(std::make_unique<AbilityHudSystem>());
        world.AddSystem(std::make_unique<PlayerNameTagSystem>());
        world.AddSystem(std::make_unique<PauseMenuSystem>());

        AudioManager::PlayMusic("lava_sound.wav", true);
        AudioManager::SetMusicVolume(1.00f);
    }

    void Interpolate(
        const GameStateBlob& previousServerState,
        const GameStateBlob& currentServerState,
        const GameStateBlob& previousLocalState,
        const GameStateBlob& currentLocalState,
        GameStateBlob& renderState,
        float serverInterpolation,
        float localInterpolation) override
    {
        const AsteroidShooterGameState& prevServer = *reinterpret_cast<const AsteroidShooterGameState*>(previousServerState.data);
        const AsteroidShooterGameState& currServer = *reinterpret_cast<const AsteroidShooterGameState*>(currentServerState.data);
        const AsteroidShooterGameState& prevLocal = *reinterpret_cast<const AsteroidShooterGameState*>(previousLocalState.data);
        const AsteroidShooterGameState& currLocal = *reinterpret_cast<const AsteroidShooterGameState*>(currentLocalState.data);

        AsteroidShooterGameState& rend = *reinterpret_cast<AsteroidShooterGameState*>(renderState.data);

        for (int i = 0; i < NUM_PLAYERS; ++i) {
            if (playerId == i)
            {
                rend.posX[i] = prevLocal.posX[i] + (currLocal.posX[i] - prevLocal.posX[i]) * localInterpolation;
                rend.posY[i] = prevLocal.posY[i] + (currLocal.posY[i] - prevLocal.posY[i]) * localInterpolation;
                float delta = currLocal.rot[i] - prevLocal.rot[i];
                while (delta > 180.0f) delta -= 360.0f;
                while (delta < -180.0f) delta += 360.0f;
                rend.rot[i] = prevLocal.rot[i] + delta * localInterpolation;
            }
            else
            {
                rend.posX[i] = prevServer.posX[i] + (currServer.posX[i] - prevServer.posX[i]) * serverInterpolation;
                rend.posY[i] = prevServer.posY[i] + (currServer.posY[i] - prevServer.posY[i]) * serverInterpolation;
                float delta = currServer.rot[i] - prevServer.rot[i];
                while (delta > 180.0f) delta -= 360.0f;
                while (delta < -180.0f) delta += 360.0f;
                rend.rot[i] = prevServer.rot[i] + delta * serverInterpolation;
            }

            rend.health[i] = currServer.health[i];
            rend.alive[i] = currServer.alive[i];
            rend.remaingShootFrames[i] = currServer.remaingShootFrames[i];
            rend.isMovingForward[i] = currServer.isMovingForward[i];
            rend.shipInclination[i] = currServer.shipInclination[i];
            rend.isShooting[i] = currServer.isShooting[i];
            rend.shootCooldown[i] = currServer.shootCooldown[i];
            rend.velX[i] = currServer.velX[i];
            rend.velY[i] = currServer.velY[i];
            rend.angularVel[i] = currServer.angularVel[i];

            // Abilities: the local player's from its own prediction, like its position, so the shield goes up and the
            // HUD reacts on the key press instead of a round trip later; everyone else's at server time.
            const AsteroidShooterGameState& abilitySrc = (playerId == i) ? currLocal : currServer;
            rend.dashTicks[i] = abilitySrc.dashTicks[i];
            rend.dashCooldown[i] = abilitySrc.dashCooldown[i];
            rend.shieldTicks[i] = abilitySrc.shieldTicks[i];
            rend.shieldCooldown[i] = abilitySrc.shieldCooldown[i];
        }

        // Bullets
        rend.bulletCount = currServer.bulletCount;
        for (int i = 0; i < MAX_BULLETS; ++i) {
            const Bullet& prevB = prevServer.bullets[i];
            const Bullet& currB = currServer.bullets[i];
            Bullet& rendB = rend.bullets[i];

            if (currB.active) {
                if (prevB.active) {
                    rendB.id = currB.id;
                    rendB.active = true;
                    rendB.posX = prevB.posX + (currB.posX - prevB.posX) * serverInterpolation;
                    rendB.posY = prevB.posY + (currB.posY - prevB.posY) * serverInterpolation;
                    rendB.velX = currB.velX;
                    rendB.velY = currB.velY;
                    rendB.ownerId = currB.ownerId;
                    rendB.lifetime = currB.lifetime;
                }
                else {
                    rendB = currB;
                }
            }
            else {
                rendB.active = false;
            }
        }

        // Walls and arena state — copy directly from server (no interpolation)
        std::memcpy(rend.hWalls, currServer.hWalls, sizeof(currServer.hWalls));
        std::memcpy(rend.vWalls, currServer.vWalls, sizeof(currServer.vWalls));
        std::memcpy(rend.cWalls, currServer.cWalls, sizeof(currServer.cWalls));
        std::memcpy(rend.tilesActive, currServer.tilesActive, sizeof(currServer.tilesActive));
        std::memcpy(rend.hWallsWarning, currServer.hWallsWarning, sizeof(currServer.hWallsWarning));
        std::memcpy(rend.vWallsWarning, currServer.vWallsWarning, sizeof(currServer.vWallsWarning));
        std::memcpy(rend.cWallsWarning, currServer.cWallsWarning, sizeof(currServer.cWallsWarning));
        std::memcpy(rend.tilesWarning, currServer.tilesWarning, sizeof(currServer.tilesWarning));

        // ...except a wall about to energise, drawn on in the local ship's (predicted) timeline: once the predicted frame
        // reaches the state frame the server turns it on in, which is when it would kill the ship where it's drawn now.
        // Otherwise it appeared framesAheadOfServer ticks late relative to the ship (see EncodeWallWarning). Only while
        // the local ship is alive: a spectator watches everyone at server time, so walls stay in step with them.
        const bool localAlive = playerId >= 0 && playerId < NUM_PLAYERS && currServer.alive[playerId];
        if (localAlive)
        {
            const int serverFrame = currentServerState.frame;
            const int localFrame = currentLocalState.frame;
            auto predictOn = [serverFrame, localFrame](bool& enabled, uint16_t& warnCode)
                {
                    if (enabled) return;
                    const int onFrame = DecodeWallOnFrame(warnCode, serverFrame);
                    if (onFrame < 0 || localFrame < onFrame) return;
                    enabled = true;
                    warnCode = WALL_WARN_NONE;
                };

            for (int x = 0; x < 2 * MAP_SIZE + 1; x++)
                for (int y = 0; y < 2 * MAP_SIZE; y++)
                    predictOn(rend.hWalls[x][y], rend.hWallsWarning[x][y]);
            for (int x = 0; x < 2 * MAP_SIZE; x++)
                for (int y = 0; y < 2 * MAP_SIZE + 1; y++)
                    predictOn(rend.vWalls[x][y], rend.vWallsWarning[x][y]);
            for (int x = 0; x < MAP_SIZE; x++)
                for (int y = 0; y < MAP_SIZE; y++)
                    for (int z = 0; z < 4; z++)
                        predictOn(rend.cWalls[x][y][z], rend.cWallsWarning[x][y][z]);
        }

        rend.startCountdownTicks = currServer.startCountdownTicks;
    }

    ~AsteroidShooterGameRenderer() override {
    }

private:
    OpenGLWindow* window;
};

#endif
