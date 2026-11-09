#ifndef AUDIO_COMPONENTS_HPP
#define AUDIO_COMPONENTS_HPP

#include <cstdint>
#include <string>
#include "AudioChannels.hpp"
#include "ecs/ecs.hpp"

struct AudioSourceComponent : public IComponent {
    std::string filePath;
    bool loop = false;
    bool play = false;
    float gain = 1.0f;
    float pitch = 1.0f;

    // Distance attenuation, in world units (AL_LINEAR_DISTANCE_CLAMPED, see AudioSystem): full gain up to
    // referenceDistance, fading linearly to silence at maxDistance. rolloffFactor scales the slope (1 = reach 0 exactly
    // at maxDistance, 0 = no attenuation). Read every tick, so they can change while the source plays.
    float referenceDistance = 1.0f;
    float maxDistance = 100.0f;
    float rolloffFactor = 1.0f;

    AudioChannel channel = AudioChannel::SFX;

    // OpenAL handles (ALuint / ALint), filled by AudioSystem. Plain integers so this component stays pure data, usable
    // from the shared game logic headers without pulling OpenAL into GameServer.
    uint32_t source = 0;
    uint32_t buffer = 0;
    bool initialized = false;
    bool pendingToDestroy = false;

    // Saved on device swap so looping sources resume from the same position
    // rather than restarting from the beginning.
    int32_t savedSampleOffset = 0;

    AudioSourceComponent() = default;

    AudioSourceComponent(const std::string& path,
        AudioChannel ch = AudioChannel::SFX,
        bool looped = false)
        : filePath(path), channel(ch), loop(looped) {
    }

    void Destroy() override {
    }
};


struct AudioListenerComponent : public IComponent {};

#endif