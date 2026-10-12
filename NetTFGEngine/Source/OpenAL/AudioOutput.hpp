#ifndef AUDIO_OUTPUT_HPP
#define AUDIO_OUTPUT_HPP

#include <atomic>
#include <algorithm>

// What the sound is played through. Picks how OpenAL Soft renders 3D positions: HRTF binaural for headphones,
// amplitude panning for speakers. Values are saved in audio_settings.cfg, so append new modes at the end.
enum class AudioOutputMode : int {
    Auto = 0,       // OpenAL Soft decides (HRTF if the OS reports headphones)
    Headphones,     // binaural HRTF
    Stereo,         // two speakers, plain panning
    Surround51,
    Surround71,
    COUNT // sentinel, keep last
};

// Lock-free for the same reason as AudioChannelManager: the settings UI writes it from the render thread while holding
// the renderer's EntityManager mutex, which the audio thread takes after audioMutex. The audio thread polls it and
// resets the device when it changes (AudioSystem::applyOutputMode).
class AudioOutputSettings {
public:
    static AudioOutputSettings& instance()
    {
        static AudioOutputSettings inst;
        return inst;
    }

    AudioOutputSettings(const AudioOutputSettings&) = delete;
    AudioOutputSettings& operator=(const AudioOutputSettings&) = delete;

    AudioOutputMode GetMode() const
    {
        return static_cast<AudioOutputMode>(mode_.load(std::memory_order_relaxed));
    }

    void SetMode(AudioOutputMode mode)
    {
        const int m = std::clamp(static_cast<int>(mode), 0, static_cast<int>(AudioOutputMode::COUNT) - 1);
        mode_.store(m, std::memory_order_relaxed);
    }

private:
    AudioOutputSettings() = default;

    std::atomic<int> mode_{ static_cast<int>(AudioOutputMode::Auto) };
};

#endif
