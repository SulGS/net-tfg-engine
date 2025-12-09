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

    AudioSourceComponent() = default;

    AudioSourceComponent(const std::string& path,
        AudioChannel ch = AudioChannel::SFX,
        bool looped = false)
        : filePath(path), channel(ch), loop(looped) {
    }

    void Destroy() override {
        if (source) {
            alDeleteSources(1, &source);
            source = 0;
        }
        if (buffer) {
            alDeleteBuffers(1, &buffer);
            buffer = 0;
        }
        initialized = false;
    }
};


struct AudioListenerComponent : public IComponent {};

#endif
