#ifndef AUDIO_COMPONENTS_HPP
#define AUDIO_COMPONENTS_HPP

#include <string>
#include <AL/al.h>
#include "AudioChannels.hpp"
#include "ecs/ecs.hpp"

struct AudioSourceComponent : public IComponent {
    std::string filePath;
    bool loop = false;
    bool play = false;
    float gain = 1.0f;

    AudioChannel channel = AudioChannel::SFX;

    ALuint source = 0;
    ALuint buffer = 0;
    bool initialized = false;
    bool pendingToDestroy = false;

    // Saved on device swap so looping sources resume from the same position
    // rather than restarting from the beginning.
    ALint savedSampleOffset = 0;

    AudioSourceComponent() = default;

    AudioSourceComponent(const std::string& path,
        AudioChannel ch = AudioChannel::SFX,
        bool looped = false)
        : filePath(path), channel(ch), loop(looped) {
    }

    void Destroy() override {
        //AudioSystem::AddPendingSourceString(source,filePath);
    }
};


struct AudioListenerComponent : public IComponent {};

#endif