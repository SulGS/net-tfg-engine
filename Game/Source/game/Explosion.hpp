#pragma once
// Ship explosion, purely visual (renderer world only): SpawnShipExplosion() builds it (particles, lighting, camera shake),
// ExplosionEffectsSystem animates/removes the blast lights, PreloadExplosionAssets() loads the sprite sheet up front.

#include <algorithm>
#include <cmath>
#include <random>
#include <string>

#include <glm/glm.hpp>

#include "ecs/ecs.hpp"
#include "ecs/ecs_common.hpp"
#include "OpenGL/Particles/ParticleEmitterComponent.hpp"
#include "OpenGL/Particles/ParticlePresets.hpp"
#include "Components.hpp"

// One blast point light: waits `delay`, then for `duration` its intensity falls exponentially from `peakIntensity`,
// colour cools colorStart->colorEnd and radius grows radiusStart->radiusEnd; then it destroys itself.
// Driven by ExplosionEffectsSystem; SpawnShipExplosion builds the set (flash, fireball, secondary, embers).
class ExplosionLight : public IComponent {
public:
	float age = 0.0f;
	float delay = 0.0f;           // seconds before it switches on
	float duration = 0.7f;        // seconds it lives once on (it eases to 0 over the last 20%)
	float peakIntensity = 110.0f;
	float decay = 8.0f;           // exponential falloff rate, 1/s
	float flicker = 0.15f;        // 0..1: how deep the fast irregular wobble goes
	float flickerPhase = 0.0f;    // desyncs the wobble between lights
	glm::vec3 colorStart = glm::vec3(1.0f);
	glm::vec3 colorEnd = glm::vec3(1.0f);
	float coolTime = 1.0f;        // seconds to go from colorStart to colorEnd
	float radiusStart = 30.0f;
	float radiusEnd = 30.0f;
	float growTime = 0.1f;        // seconds to go from radiusStart to radiusEnd
};

// Tunables for SpawnShipExplosion.
struct ExplosionSettings {
	float scale = 3.5f;             // overall size: emitter scale and particle reach both follow it
	float cameraTrauma = 0.75f;     // 0..1 shake added to the camera at zero distance

	// Blast lighting. Intensities are in the scene's units (light = intensity / distance^2, times the surface albedo / pi): the floor sits ~9 below the flash and ~8 below the fireball.
	float lightHeight = 5.0f;       // how far above the play plane the flash light sits (avoids 1/d^2 blow-out); the other lights are placed relative to it
	float flashLight = 500.0f;      // detonation flash: white-hot, gone in ~0.15 s, reaches across the arena
	float fireLight = 220.0f;       // fireball: orange, ~1.5 s, cools towards red
	float secondaryLight = 130.0f;  // each of the smaller blasts that go off across the wreck
	int   secondaryLights = 4;      // how many; they are scattered in time (0.12-0.57 s, like the secondary particle layer) and space
	float emberLight = 30.0f;       // the wreck glowing red-hot afterwards, ~3.5 s
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

// Creates one blast light at `position` from its envelope. It starts switched off
// (zero intensity and radius) and ExplosionEffectsSystem takes it from there.
inline void SpawnBlastLight(EntityManager& em, const glm::vec3& position, const ExplosionLight& envelope)
{
	Entity lightEntity = em.CreateEntity();
	Transform* lt = em.AddComponent<Transform>(lightEntity, Transform{});
	lt->setPosition(position);

	PointLightComponent* light = em.AddComponent<PointLightComponent>(lightEntity, PointLightComponent{});
	light->color = envelope.colorStart;
	light->intensity = 0.0f;
	light->radius = 0.0f;
	// Shadow cube maps are few and expensive, and a blast this short would not be worth one.
	light->castShadows = false;

	em.AddComponent<ExplosionLight>(lightEntity, envelope);
}

// Blast lighting in stages following the particle layers: flash (white-hot, ~0.15 s, arena-wide), fireball (~1.5 s,
// yellow-white to deep red), secondary (smaller blasts across the wreck), embers (red-hot wreck long after the fire).
inline void SpawnBlastLighting(EntityManager& em, const glm::vec3& position, const ExplosionSettings& settings)
{
	static thread_local std::mt19937 rng{ std::random_device{}() };
	auto random01 = [&]() { return std::uniform_real_distribution<float>(0.0f, 1.0f)(rng); };

	const glm::vec3 above(0.0f, 0.0f, settings.lightHeight);

	// Flash. Smooth (no flicker): it is over before a flicker would read.
	{
		ExplosionLight l;
		l.duration = 0.25f;
		l.peakIntensity = settings.flashLight;
		l.decay = 28.0f;
		l.flicker = 0.0f;
		l.colorStart = glm::vec3(1.0f, 0.93f, 0.8f);
		l.colorEnd = glm::vec3(1.0f, 0.7f, 0.35f);
		l.coolTime = 0.15f;
		l.radiusStart = l.radiusEnd = 70.0f;
		SpawnBlastLight(em, position + above, l);
	}

	// Fireball. The radius swells quickly as the fire expands.
	{
		ExplosionLight l;
		l.duration = 1.8f;
		l.peakIntensity = settings.fireLight;
		l.decay = 2.0f;
		l.flicker = 0.3f;
		l.flickerPhase = random01() * 6.28f;
		l.colorStart = glm::vec3(1.0f, 0.78f, 0.38f);
		l.colorEnd = glm::vec3(0.9f, 0.22f, 0.04f);
		l.coolTime = 1.4f;
		l.radiusStart = 35.0f;
		l.radiusEnd = 60.0f;
		l.growTime = 0.3f;
		SpawnBlastLight(em, position + above - glm::vec3(0.0f, 0.0f, 1.0f), l);
	}

	// Secondary blasts: scattered in time and over the wreck, like the secondary
	// particle layer (delay 0.12 s, spawning for ~0.45 s).
	for (int i = 0; i < settings.secondaryLights; ++i)
	{
		const float angle = random01() * 6.28318f;
		const float reach = 4.5f * std::sqrt(random01());

		ExplosionLight l;
		l.delay = 0.12f + random01() * 0.45f;
		l.duration = 0.3f;
		l.peakIntensity = settings.secondaryLight * (0.7f + 0.6f * random01());
		l.decay = 14.0f;
		l.flicker = 0.25f;
		l.flickerPhase = random01() * 6.28f;
		l.colorStart = glm::vec3(1.0f, 0.7f, 0.3f);
		l.colorEnd = glm::vec3(1.0f, 0.4f, 0.1f);
		l.coolTime = 0.2f;
		l.radiusStart = l.radiusEnd = 30.0f;
		SpawnBlastLight(em, position + glm::vec3(std::cos(angle) * reach, std::sin(angle) * reach, 3.0f), l);
	}

	// Embers: low and slow, a red glow on the floor around the wreck.
	{
		ExplosionLight l;
		l.duration = 3.5f;
		l.peakIntensity = settings.emberLight;
		l.decay = 0.8f;
		l.flicker = 0.35f;
		l.flickerPhase = random01() * 6.28f;
		l.colorStart = glm::vec3(0.95f, 0.22f, 0.05f);
		l.colorEnd = glm::vec3(0.5f, 0.07f, 0.01f);
		l.coolTime = 3.0f;
		l.radiusStart = l.radiusEnd = 22.0f;
		SpawnBlastLight(em, position + glm::vec3(0.0f, 0.0f, 1.5f), l);
	}
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

	// Lighting: lights the arena around the wreck through the whole blast. The lights are held above the play plane so nearby surfaces are not overexposed by 1/d^2.
	SpawnBlastLighting(em, position, settings);

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

// Animates the blast lights (see ExplosionLight) and removes them when they are done. Add it to any renderer world that spawns explosions.
class ExplosionEffectsSystem : public ISystem
{
public:
	void Update(EntityManager& entityManager, std::vector<EventEntry>& events,
		bool isServer, float deltaTime) override
	{
		auto lightQuery = entityManager.CreateQuery<PointLightComponent, ExplosionLight>();
		for (auto [entity, light, blast] : lightQuery)
		{
			blast->age += deltaTime;
			const float t = blast->age - blast->delay;

			if (t >= blast->duration)
			{
				entityManager.DestroyEntity(entity);
				continue;
			}

			// Not on yet: zero radius makes the shader skip it on its first test.
			if (t < 0.0f)
			{
				light->intensity = 0.0f;
				light->radius = 0.0f;
				continue;
			}

			// Two incommensurate sines: an irregular wobble rather than a metronome.
			const float wobble = 0.5f + 0.25f * std::sin(t * 63.0f + blast->flickerPhase)
			                          + 0.25f * std::sin(t * 97.0f + blast->flickerPhase * 2.1f);
			const float flicker = 1.0f - blast->flicker * (1.0f - wobble);

			// Ease to zero over the last 20% so it never pops off.
			const float endFade = 1.0f - smooth01((t / blast->duration - 0.8f) / 0.2f);

			light->intensity = blast->peakIntensity * std::exp(-blast->decay * t) * flicker * endFade;
			light->color = glm::mix(blast->colorStart, blast->colorEnd, smooth01(t / blast->coolTime));

			const float grow = std::clamp(t / blast->growTime, 0.0f, 1.0f);
			light->radius = glm::mix(blast->radiusStart, blast->radiusEnd, 1.0f - (1.0f - grow) * (1.0f - grow));
		}
	}

private:
	static float smooth01(float x)
	{
		x = std::clamp(x, 0.0f, 1.0f);
		return x * x * (3.0f - 2.0f * x);
	}
};
