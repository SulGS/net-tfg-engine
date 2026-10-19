#pragma once

#include <algorithm>
#include <cmath>
#include <glm/glm.hpp>

#include "ecs/ecs_common.hpp"
#include "OpenAL/AudioComponents.hpp"
#include "../Components.hpp"

// Drives each ship's engine loop (engine.wav) from two smoothed inputs:
//   throttle  - whether the ship is thrusting (isMovingForward): the main cue, what the player is doing right now.
//   speedNorm - |velocity| / max speed: a background hum that keeps rising during propulsion (speed > max).
// Pitch is mapped in semitones and gain in dB, since that is how both are heard; a linear 0.5..1.2 pitch spent most of
// its range sounding like a slowed-down tape. Both inputs are eased toward their targets (fast attack, slower release):
// they come from snapshots / rollback and jumped every tick, which zippered the pitch.
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
		// A hitch (loading, debugger) must not make the ease jump past its target.
		const float dt = std::clamp(deltaTime, 0.0f, 0.1f);

		auto audioQuery = entityManager.CreateQuery<Transform, AudioSourceComponent, ThrusterSound>();
		auto shipQuery = entityManager.CreateQuery<Transform, Playable, SpaceShip>();
		for (auto [audioEntity, soundT, audio, snd] : audioQuery)
		{
			for (auto [shipEntity, shipT, play, ship] : shipQuery)
			{
				if (snd->shipEntity != play->playerId) continue;

				soundT->setPosition(shipT->getPosition());

				if (ship->isAlive)
				{
					// Starts the loop if it isn't running; AudioSystem doesn't restart a loop that already plays.
					audio->play = true;

					const float speed = glm::length(glm::vec2(ship->velX, ship->velY));
					Approach(snd->throttle, ship->isMovingForward ? 1.0f : 0.0f, THROTTLE_ATTACK, THROTTLE_RELEASE, dt);
					Approach(snd->speedNorm, std::min(speed / SHIP_MAX_SPEED, 1.5f), SPEED_ATTACK, SPEED_RELEASE, dt);
					Approach(snd->aliveFade, 1.0f, FADE_IN, FADE_IN, dt);
				}
				else
				{
					// Fade out instead of cutting (the explosion covers it). The source keeps looping silently.
					Approach(snd->aliveFade, 0.0f, FADE_OUT, FADE_OUT, dt);
					if (snd->aliveFade < 0.01f)
					{
						// Fully silent: respawn starts from idle, not from the speed it died at.
						snd->aliveFade = 0.0f;
						snd->throttle = 0.0f;
						snd->speedNorm = 0.0f;
					}
				}

				// Idle -4 st .. full speed +1 st, thrusting +2 st on top (up to ~+5.5 st in a propulsion burst).
				const float semitones = -4.0f + 5.0f * snd->speedNorm + 2.0f * snd->throttle;
				audio->pitch = std::exp2(semitones / 12.0f);

				// Idle -30 dB, coasting at full speed -16 dB, thrusting at full speed -6 dB (the old 0.5 peak).
				const float db = std::min(-30.0f + 14.0f * snd->speedNorm + 10.0f * snd->throttle, -6.0f);
				audio->gain = std::pow(10.0f, db / 20.0f) * snd->aliveFade;
				break;
			}
		}
	}

private:
	static constexpr float SHIP_MAX_SPEED = 3.0f;      // InputSystem's MAX_SPEED (cruise cap; propulsion exceeds it)

	// Time constants, seconds.
	static constexpr float THROTTLE_ATTACK = 0.06f;
	static constexpr float THROTTLE_RELEASE = 0.25f;
	static constexpr float SPEED_ATTACK = 0.15f;
	static constexpr float SPEED_RELEASE = 0.40f;
	static constexpr float FADE_IN = 0.20f;
	static constexpr float FADE_OUT = 0.30f;

	// Frame-rate independent exponential ease toward target, with separate rise / fall time constants.
	static void Approach(float& v, float target, float riseTau, float fallTau, float dt)
	{
		const float tau = target > v ? riseTau : fallTau;
		v += (target - v) * (1.0f - std::exp(-dt / tau));
	}
};
