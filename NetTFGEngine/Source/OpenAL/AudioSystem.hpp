#ifndef AUDIO_SYSTEM_HPP
#define AUDIO_SYSTEM_HPP

#include "ecs/ecs.hpp"
#include "ecs/ecs_common.hpp"
#include "AudioComponents.hpp"
#include "AudioChannels.hpp"

#include "Utils/Debug/Debug.hpp"

#include <AL/alc.h>
#include <AL/al.h>
#include <AL/alext.h>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <vector>
#include <cstdio>
#include <cstring>

/* ===========================================================
   dr_wav WAV loader (header-only)
   =========================================================== */
#define DR_WAV_IMPLEMENTATION
#include "dr_wav.h"

   /* ===========================================================
      AUDIO SYSTEM
      =========================================================== */
class AudioSystem : public ISystem {
public:

    AudioSystem() {
        initializeOpenAL();
        initMusicSource();
    }

    ~AudioSystem() {
        shutdownOpenAL();
    }

    AudioChannelManager channels;

    /* --------------------------
        MUSIC CONTROL API
       -------------------------- */
    void PlayMusic(const std::string& file, bool loop = true) {
        ALenum format;
        ALsizei freq;

        if (musicBuffer != 0) {
            alSourceStop(musicSource);
            alSourcei(musicSource, AL_BUFFER, 0);
            alDeleteBuffers(1, &musicBuffer);
            musicBuffer = 0;
        }

        if (!loadWav(file.c_str(), musicBuffer, format, freq)) {
            Debug::Error("AudioSystem") << "Failed to load music WAV: " << file << "\n";
            return;
        }

        alSourcei(musicSource, AL_BUFFER, musicBuffer);
        alSourcei(musicSource, AL_LOOPING, loop ? AL_TRUE : AL_FALSE);

        alSourcePlay(musicSource);
        Debug::Info("AudioSystem") << "Music playing: " << file << "\n";
    }

    void StopMusic() {
        alSourceStop(musicSource);
    }

    void SetMusicVolume(float volume) {
        musicVolume = glm::clamp(volume, 0.0f, 1.0f);
        alSourcef(musicSource, AL_GAIN, musicVolume);
    }

    /* -----------------------------
        ECS UPDATE
       ----------------------------- */
    void Update(EntityManager& entityManager,
        std::vector<EventEntry>&,
        bool,
        float) override
    {
        /* -----------------------------
           1. LISTENER UPDATE
           ----------------------------- */
        Transform* listenerT = nullptr;

        auto listenerQuery = entityManager.CreateQuery<AudioListenerComponent, Transform>();
        for (auto [ent, listener, t] : listenerQuery) {
            listenerT = t;
            break;
        }

        if (listenerT) {
            setListenerFromTransform(*listenerT);
        }

        /* -----------------------------
           2. 3D SOURCES
           ----------------------------- */
        auto sourceQuery = entityManager.CreateQuery<AudioSourceComponent, Transform>();

        for (auto [ent, audio, t] : sourceQuery) {

            if (!audio->initialized)
                initializeSource(*audio);

            updateSourceTransform(*audio, *t);

            float finalGain = audio->gain * channels.GetVolume(audio->channel);
            alSourcef(audio->source, AL_GAIN, finalGain);

            if (audio->play) {
                alSourcePlay(audio->source);
                audio->play = false;
            }
        }
    }

private:

    /* ===========================================================
       OPENAL CONTEXT
       =========================================================== */
    ALCdevice* device = nullptr;
    ALCcontext* context = nullptr;

    bool initializeOpenAL() {
        device = alcOpenDevice(nullptr);
        if (!device) {
            Debug::Error("AudioSystem") << "Cannot open audio device\n";
            return false;
        }

        ALCint attrs[] = { ALC_HRTF_SOFT, ALC_TRUE, 0 };

        context = alcCreateContext(device, attrs);
        if (!context) {
            context = alcCreateContext(device, nullptr);
        }

        if (!context || !alcMakeContextCurrent(context)) {
            Debug::Error("AudioSystem") << "Failed to create OpenAL context\n";
            return false;
        }

        Debug::Info("AudioSystem") << "OpenAL initialized.\n";
        return true;
    }

    void shutdownOpenAL() {
        if (musicSource) alDeleteSources(1, &musicSource);
        if (musicBuffer) alDeleteBuffers(1, &musicBuffer);

        alcMakeContextCurrent(nullptr);

        if (context) alcDestroyContext(context);
        if (device) alcCloseDevice(device);

        Debug::Info("AudioSystem") << "OpenAL shutdown complete\n";
    }

    /* ===========================================================
       LISTENER
       =========================================================== */
    void setListenerFromTransform(const Transform& t) {
        glm::vec3 pos = t.getPosition();
        glm::quat rot = glm::quat(glm::radians(t.getRotation()));

        glm::vec3 fwd = rot * glm::vec3(0, 0, -1);
        glm::vec3 up = rot * glm::vec3(0, 1, 0);

        ALfloat ori[] = { fwd.x, fwd.y, fwd.z, up.x, up.y, up.z };

        alListener3f(AL_POSITION, pos.x, pos.y, pos.z);
        alListenerfv(AL_ORIENTATION, ori);
    }

    /* ===========================================================
       MUSIC SOURCE
       =========================================================== */
    ALuint musicSource = 0;
    ALuint musicBuffer = 0;
    float musicVolume = 1.0f;

    void initMusicSource() {
        alGenSources(1, &musicSource);
        alSourcef(musicSource, AL_GAIN, 1.0f);
        alSourcei(musicSource, AL_LOOPING, AL_TRUE);
        Debug::Info("AudioSystem") << "Music source initialized\n";
    }

    /* ===========================================================
       3D AUDIO SOURCES
       =========================================================== */
    void initializeSource(AudioSourceComponent& ac) {
        ALenum format;
        ALsizei freq;

        if (!loadWav(ac.filePath.c_str(), ac.buffer, format, freq)) {
            Debug::Error("AudioSystem") << "Failed to load WAV: " << ac.filePath << "\n";
            return;
        }

        alGenSources(1, &ac.source);
        alSourcei(ac.source, AL_BUFFER, ac.buffer);
        alSourcei(ac.source, AL_LOOPING, ac.loop ? AL_TRUE : AL_FALSE);

        ac.initialized = true;
    }

    void updateSourceTransform(AudioSourceComponent& ac, const Transform& t) {
        glm::vec3 pos = t.getPosition();
        alSource3f(ac.source, AL_POSITION, pos.x, pos.y, pos.z);
    }

    /* ===========================================================
       WAV LOADER (dr_wav)
       =========================================================== */
    bool loadWav(const char* file, ALuint& buffer,
        ALenum& formatOut, ALsizei& freqOut)
    {
        unsigned int channels;
        unsigned int sampleRate;
        drwav_uint64 totalPCMFrames;

        float* pcmData = drwav_open_file_and_read_pcm_frames_f32(
            file, &channels, &sampleRate, &totalPCMFrames, nullptr);

        if (!pcmData) {
            Debug::Error("AudioSystem") << "Failed to load WAV: " << file << "\n";
            return false;
        }

        ALenum format =
            (channels == 1) ? AL_FORMAT_MONO_FLOAT32 :
            (channels == 2) ? AL_FORMAT_STEREO_FLOAT32 :
            AL_NONE;

        if (format == AL_NONE) {
            Debug::Error("AudioSystem") << "Unsupported WAV channel count: " << channels << "\n";
            drwav_free(pcmData, nullptr);
            return false;
        }

        alGenBuffers(1, &buffer);
        alBufferData(buffer, format,
            pcmData,
            totalPCMFrames * channels * sizeof(float),
            sampleRate);

        drwav_free(pcmData, nullptr);

        formatOut = format;
        freqOut = sampleRate;

        return true;
    }
};

#endif
