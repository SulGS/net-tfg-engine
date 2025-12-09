#ifndef AUDIO_CHANNELS_HPP
#define AUDIO_CHANNELS_HPP

#include <unordered_map>
#include <string>

enum class AudioChannel {
    MASTER,
    MUSIC,
    SFX,
    VOICE,
    UI
};

struct AudioChannelData {
    float volume = 1.0f;
};

class AudioChannelManager {
public:
    std::unordered_map<AudioChannel, AudioChannelData> channels;

    AudioChannelManager() {
        channels[AudioChannel::MASTER].volume = 1.0f;
        channels[AudioChannel::MUSIC].volume = 1.0f;
        channels[AudioChannel::SFX].volume = 1.0f;
        channels[AudioChannel::VOICE].volume = 1.0f;
        channels[AudioChannel::UI].volume = 1.0f;
    }

    float GetVolume(AudioChannel channel) {
        return channels[channel].volume * channels[AudioChannel::MASTER].volume;
    }

    void SetVolume(AudioChannel channel, float vol) {
        channels[channel].volume = std::clamp(vol, 0.0f, 1.0f);
    }
};

#endif
