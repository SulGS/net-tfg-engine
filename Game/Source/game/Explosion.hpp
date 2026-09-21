#pragma once
// Ship explosion: everything the player sees when a ship dies, in one place.
//
//   SpawnShipExplosion()      builds the effect (particle layers, flash light, camera shake)
//   ExplosionEffectsSystem    fades the flash light out
//   PreloadExplosionAssets()  loads the explosion sprite sheet up front so the first death does not stall
//
// Purely visual: it lives in the renderer world and never touches simulated game state.

#include <algorithm>
#include <cmath>
#include <string>

#include <glm/glm.hpp>

#include "ecs/ecs.hpp"
#include "ecs/ecs_common.hpp"
#include "OpenGL/Particles/ParticleEmitterComponent.hpp"
#include "OpenGL/Particles/ParticlePresets.hpp"
#include "Components.hpp"

// Point light that flashes with the blast, then decays and destroys itself.
class ExplosionLight : public IComponent {
public:
	float age;
	float duration;
	float peakIntensity;
	ExplosionLight() : age(0.0f), duration(0.7f), peakIntensity(110.0f) {}
	ExplosionLight(float dur, float peak) : age(0.0f), duration(dur), peakIntensity(peak) {}
};

// Tunables for SpawnShipExplosion.
struct ExplosionSettings {
	float scale = 3.5f;             // overall size: emitter scale and particle reach both follow it
	float cameraTrauma = 0.75f;     // 0..1 shake added to the camera at zero distance
	float lightPeak = 110.0f;       // flash light intensity
	float lightHeight = 5.0f;       // how far above the play plane the flash light sits (avoids 1/d^2 blow-out)
	float height = 4.0f;            // how far above the play plane the explosion is raised. The camera looks at the arena at an angle and the sprite faces it, so its lower half would sink into the floor. The lift is applied along the line to the camera, and the size is compensated, so on screen it stays where it was and the same size

	// Sprite sheet that plays the body of the explosion (fireball -> smoke): 10x10 frames, read left-to-right, top-to-bottom, with a real alpha channel.
	// Set body.texture = "" to fall back to the procedural fireball and soot. The sheet must be listed in the scene's .ntfg.
	ParticlePresets::FlipbookSettings body = MakeDefaultBody();

	static ParticlePresets::FlipbookSettings MakeDefaultBody()
	{
		ParticlePresets::FlipbookSettings fb;
		fb.texture = "explosion_spritesheet.png";
		fb.cols = 10;
		fb.rows = 10;
		fb.alpha = FlipbookAlpha::TextureAlpha;
		fb.centerOffset = glm::vec2(0.0f, -0.13f);   // this sheet's explosion is born ~13% below the cell centre
		return fb;
	}
};

inline ExplosionSettings g_explosionSettings;

inline void RegisterExplosionComponents(EntityManager& em)
{
	em.RegisterComponentType<ExplosionLight>();
	em.RegisterComponentType<ExplosionPlayerID>();
}

// The sprite sheet is decompressed and uploaded the first time an emitter references it, which for a sheet this size takes a noticeable moment. An always-disabled emitter that references it makes that happen when the scene starts instead of at the first death.
inline void PreloadExplosionAssets(EntityManager& em, const ExplosionSettings& settings = g_explosionSettings)
{
	if (settings.body.texture.empty()) return;

	Entity e = em.CreateEntity();
	em.AddComponent<Transform>(e, Transform{});

	ParticleEmitterComponent preload;
	preload.enabled = false;
	preload.looping = true;
	preload.maxParticles = 1;
	ParticlePresets::ApplyFlipbook(preload, settings.body);
	em.AddComponent<ParticleEmitterComponent>(e, preload);
}

// Builds the whole explosion at `position`. `playerId` tags every entity so the game can clean them up together.
inline void SpawnShipExplosion(EntityManager& em, const glm::vec3& position, int playerId,
	const ExplosionSettings& settings = g_explosionSettings)
{
	// Camera basis at the moment of the blast: where the camera is, and which way is "up" and "right" on screen.
	glm::vec3 camRight(1.0f, 0.0f, 0.0f), camUp(0.0f, 1.0f, 0.0f), toCamera(0.0f, 0.0f, 1.0f);
	float cameraDistance = 0.0f;
	{
		auto camQuery = em.CreateQuery<Camera, Transform>();
		for (auto [camEntity, camera, camTransform] : camQuery)
		{
			const glm::vec3 camPos = camTransform->getPosition();
			const glm::vec3 forward = glm::normalize(camera->getTarget() - camPos);
			camRight = glm::normalize(glm::cross(forward, camera->getUp()));
			camUp = glm::cross(camRight, forward);
			toCamera = glm::normalize(camPos - position);
			cameraDistance = glm::length(camPos - position);
			break;
		}
	}

	// Raise the explosion `height` above the play plane by sliding it toward the camera, then shrink it by the same perspective factor: it lands on the same pixels at the same size, only clear of the floor.
	const float lift = (cameraDistance > 0.0f) ? settings.height / std::max(toCamera.z, 0.25f) : 0.0f;
	const float sizeFactor = (cameraDistance > lift + 1.0f) ? (cameraDistance - lift) / cameraDistance : 1.0f;
	const glm::vec3 origin = position + toCamera * lift;
	const float scale = settings.scale * sizeFactor;

	// Particle layers (flash, body, sparks, fine debris, secondary blasts, refraction ring).
	ParticlePresets::ExplosionOptions options;
	options.scale = scale;
	options.body = settings.body;
	options.viewRight = camRight;
	options.viewUp = camUp;

	for (auto& layer : ParticlePresets::MakeExplosion(options))
	{
		Entity e = em.CreateEntity();
		Transform* t = em.AddComponent<Transform>(e, Transform{});
		t->setPosition(origin);
		t->setScale(glm::vec3(scale));
		em.AddComponent<ParticleEmitterComponent>(e, layer);
		em.AddComponent<ExplosionPlayerID>(e, ExplosionPlayerID{ playerId });
	}

	// Flash light: lights the wreck and the arena around it. Held above the play plane so nearby surfaces are not overexposed by 1/d^2.
	{
		Entity lightEntity = em.CreateEntity();
		Transform* lt = em.AddComponent<Transform>(lightEntity, Transform{});
		lt->setPosition(position + glm::vec3(0.0f, 0.0f, settings.lightHeight));
		PointLightComponent* light = em.AddComponent<PointLightComponent>(lightEntity, PointLightComponent{});
		light->color = glm::vec3(1.0f, 0.6f, 0.25f);
		light->intensity = settings.lightPeak;
		light->radius = 30.0f;
		light->castShadows = false;
		em.AddComponent<ExplosionLight>(lightEntity, ExplosionLight{ 0.7f, settings.lightPeak });
	}

	// Camera shake, weaker the further the blast is from what the camera is looking at.
	auto camQuery = em.CreateQuery<Camera, Transform>();
	for (auto [camEntity, camera, camTransform] : camQuery)
	{
		const glm::vec3 look = camera->getTarget();
		const float dx = look.x - position.x;
		const float dy = look.y - position.y;
		const float d = std::sqrt(dx * dx + dy * dy) / 12.0f;
		camera->addTrauma(settings.cameraTrauma / (1.0f + d * d));
		break;
	}
}

// Fades flash lights out. Add it to any renderer world that spawns explosions.
class ExplosionEffectsSystem : public ISystem
{
public:
	void Update(EntityManager& entityManager, std::vector<EventEntry>& events,
		bool isServer, float deltaTime) override
	{
		// Fast exponential falloff with a slight flicker, then remove.
		auto lightQuery = entityManager.CreateQuery<PointLightComponent, ExplosionLight>();
		for (auto [entity, light, blast] : lightQuery)
		{
			blast->age += deltaTime;
			if (blast->age >= blast->duration)
			{
				entityManager.DestroyEntity(entity);
				continue;
			}
			const float flicker = 0.85f + 0.15f * std::sin(blast->age * 70.0f);
			light->intensity = blast->peakIntensity * std::exp(-8.0f * blast->age) * flicker;
		}
	}
};
