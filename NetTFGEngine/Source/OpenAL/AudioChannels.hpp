#ifndef AUDIO_CHANNELS_HPP
#define AUDIO_CHANNELS_HPP

#include <array>
#include <atomic>
#include <algorithm>
#include <cstddef>

enum class AudioChannel {
    MASTER,
    MUSIC,
    SFX,
    VOICE,
    UI,
    COUNT // sentinel, keep last
};

// Lock-free on purpose: a mutex here was also taken (in the opposite order) by the audio thread while holding the
// renderer's EntityManager mutex, so a UI volume slider running under that mutex could deadlock against it.
// Plain atomics avoid the lock-ordering problem entirely.
class AudioChannelManager {
public:
    static AudioChannelManager& instance()
    {
        static AudioChannelManager inst;
        return inst;
    }

    AudioChannelManager(const AudioChannelManager&) = delete;
    AudioChannelManager& operator=(const AudioChannelManager&) = delete;

    // Effective playback gain: channel volume scaled by master. Never call
    // with AudioChannel::MASTER itself, or master gets applied twice.
    float GetVolume(AudioChannel channel) const
    {
        return GetVolumeRaw(channel) * GetVolumeRaw(AudioChannel::MASTER);
    }

    // Raw, unscaled channel volume — what a settings UI should read/write,
    // so moving the master slider doesn't visually move every other slider.
    float GetVolumeRaw(AudioChannel channel) const
    {
        return volumes_[static_cast<size_t>(channel)].load(std::memory_order_relaxed);
    }

    void SetVolume(AudioChannel channel, float vol)
    {
        volumes_[static_cast<size_t>(channel)].store(
            std::clamp(vol, 0.0f, 1.0f), std::memory_order_relaxed);
    }

private:
    AudioChannelManager()
    {
        for (auto& v : volumes_)
            v.store(1.0f, std::memory_order_relaxed);
    }

    std::array<std::atomic<float>, static_cast<size_t>(AudioChannel::COUNT)> volumes_;
};

#endif
