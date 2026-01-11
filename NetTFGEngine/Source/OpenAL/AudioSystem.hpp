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
#include <unordered_set>
#include <string>

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
        initializeOpenAL();   // opens default device (effective default)
        initMusicSource();
    }

    ~AudioSystem() {
        shutdownOpenAL();
    }

    AudioChannelManager channels;

    /* --------------------------
        DEVICE / ENUMERATION API
       -------------------------- */

       // Returns list of available playback devices (OS names exposed to OpenAL)
    std::vector<std::string> GetAvailableDevices() {
        std::vector<std::string> devices;

        // If enumeration extension available, use ALL_DEVICES_SPECIFIER
        if (alcIsExtensionPresent(nullptr, "ALC_ENUMERATION_EXT") == AL_FALSE) {
            // enumeration not supported: return default device string if present
            const ALCchar* def = alcGetString(nullptr, ALC_DEFAULT_DEVICE_SPECIFIER);
            if (def) devices.emplace_back(reinterpret_cast<const char*>(def));
            return devices;
        }

        const ALCchar* deviceList = alcGetString(nullptr, ALC_ALL_DEVICES_SPECIFIER);
        if (!deviceList) return devices;

        const ALCchar* ptr = deviceList;
        while (*ptr) {
            devices.emplace_back(std::string(ptr));
            ptr += strlen(ptr) + 1;
        }
        return devices;
    }

    // The name we stored when opening the device (most reliable)
    std::string GetCurrentDevice() {
        if (!currentDeviceName.empty()) return currentDeviceName;

        // fallback: try device specifier (may return "OpenAL Soft")
        if (device) {
            const ALCchar* ds = alcGetString(device, ALC_DEVICE_SPECIFIER);
            if (ds) return std::string(reinterpret_cast<const char*>(ds));
        }
        return std::string();
    }

    // Returns the default device string reported by OpenAL.
    // Note: on some systems/implementations this may be "OpenAL Soft"
    std::string GetDefaultDevice() {
        const ALCchar* defaultDevice = alcGetString(nullptr, ALC_DEFAULT_DEVICE_SPECIFIER);
        return defaultDevice ? std::string(reinterpret_cast<const char*>(defaultDevice)) : std::string();
    }

    // Returns the effective default device name as exposed by enumeration:
    // - If enumeration available, the first string in ALC_ALL_DEVICES_SPECIFIER is typically the OS default.
    // - Fallback to ALC_DEFAULT_DEVICE_SPECIFIER if enumeration not available.
    std::string GetEffectiveDefaultDevice() {
        auto list = GetAvailableDevices();
        if (!list.empty()) return list.front();
        return GetDefaultDevice();
    }

    // Returns true if the system default device (effective) is different from the one we opened.
    bool HasDeviceChanged() {
        std::string current = GetCurrentDevice();
        std::string effectiveDefault = GetEffectiveDefaultDevice();

        if (current.empty() || effectiveDefault.empty()) return false;
        return current != effectiveDefault;
    }

    // Switch to whatever OpenAL considers the effective default device (via enumeration fallback)
    bool SwitchToDefaultDevice() {
        std::string defaultDevice = GetEffectiveDefaultDevice();
        if (defaultDevice.empty()) {
            Debug::Warning("AudioSystem") << "No effective default device available to switch to.\n";
            return false;
        }
        return ChangeOutputDevice(defaultDevice);
    }

    // Change to a device by name. If open fails, attempts to open NULL (implementation default).
    bool ChangeOutputDevice(const std::string& deviceName) {

        // Save music playing state
        bool wasMusicPlaying = false;
        ALint musicState = AL_STOPPED;
        if (musicSource) {
            alGetSourcei(musicSource, AL_SOURCE_STATE, &musicState);
            wasMusicPlaying = (musicState == AL_PLAYING);
        }

        // Save current music file/loop so we can restart after switching
        std::string savedMusicFile = lastMusicFile;
        bool savedMusicLoop = lastMusicLoop;
        float savedMusicVolume = musicVolume;

        // Shutdown current device (but keep state saved above)
        shutdownOpenAL();

        // Try to open requested device
        ALCdevice* newDevice = alcOpenDevice(deviceName.c_str());
        if (!newDevice) {
            Debug::Warning("AudioSystem") << "Failed to open requested device '" << deviceName << "'. Trying implementation default (nullptr).\n";
            newDevice = alcOpenDevice(nullptr);
            if (!newDevice) {
                Debug::Error("AudioSystem") << "Failed to open any device while switching.\n";
                return false;
            }
            else {
                Debug::Warning("AudioSystem") << "Opened implementation default device instead.\n";
            }
        }

        // Create context (try HRTF attr first)
        ALCint attrs[] = { ALC_HRTF_SOFT, ALC_TRUE, 0 };
        ALCcontext* newContext = alcCreateContext(newDevice, attrs);
        if (!newContext) {
            Debug::Warning("AudioSystem") << "HRTF context creation failed on new device, falling back to normal context.\n";
            newContext = alcCreateContext(newDevice, nullptr);
            if (!newContext) {
                Debug::Error("AudioSystem") << "Failed to create context on new device\n";
                alcCloseDevice(newDevice);
                return false;
            }
        }

        if (!alcMakeContextCurrent(newContext)) {
            Debug::Error("AudioSystem") << "Failed to make new context current\n";
            alcDestroyContext(newContext);
            alcCloseDevice(newDevice);
            return false;
        }

        // Assign to members (device/context were cleaned in shutdownOpenAL())
        device = newDevice;
        context = newContext;

        // Store the real chosen device name. Prefer enumeration-exposed name if it matches deviceName,
        // otherwise store what we were asked to open (or implementation provided name).
        currentDeviceName = deviceName;

        // Re-init music source and other state
        initMusicSource();
        needsReinit = true;

        // Restore music if needed
        musicVolume = savedMusicVolume;
        if (wasMusicPlaying && !savedMusicFile.empty()) {
            PlayMusic(savedMusicFile, savedMusicLoop);
        }

        Debug::Info("AudioSystem") << "Device switched successfully to: " << currentDeviceName << "\n";
        return true;
    }

    /* --------------------------
        MUSIC CONTROL API
       -------------------------- */
    void PlayMusic(const std::string& file, bool loop = true) {
        // delete previous music buffer if present
        if (musicBuffer != 0) {
            alSourceStop(musicSource);
            alSourcei(musicSource, AL_BUFFER, 0);
            alDeleteBuffers(1, &musicBuffer);
            musicBuffer = 0;
        }

        ALenum format;
        ALsizei freq;

        if (!loadWav(file.c_str(), musicBuffer, format, freq)) {
            Debug::Error("AudioSystem") << "Failed to load music WAV: " << file << "\n";
            return;
        }

        alSourcei(musicSource, AL_BUFFER, musicBuffer);
        alSourcei(musicSource, AL_LOOPING, loop ? AL_TRUE : AL_FALSE);
        alSourcef(musicSource, AL_GAIN, musicVolume);

        alSourcePlay(musicSource);

        // Store for device change recovery
        lastMusicFile = file;
        lastMusicLoop = loop;

        Debug::Info("AudioSystem") << "Music playing: " << file << "\n";
    }

    void StopMusic() {
        if (musicSource) alSourceStop(musicSource);
        lastMusicFile.clear();
    }

    void SetMusicVolume(float volume) {
        musicVolume = glm::clamp(volume, 0.0f, 1.0f);
        if (musicSource) alSourcef(musicSource, AL_GAIN, musicVolume);
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
           0. CHECK FOR DEVICE CHANGES
           ----------------------------- */
        if (HasDeviceChanged()) {
            Debug::Info("AudioSystem") << "Default audio device changed (effective). Switching to it.\n";
            SwitchToDefaultDevice();
        }

        /* -----------------------------
           1. REINITIALIZE AFTER DEVICE CHANGE
           ----------------------------- */
        if (needsReinit) {
            // Clear all audio sources - they'll be recreated below
            for (Entity ent : activeAudioEntities) {
                auto* audio = entityManager.GetComponent<AudioSourceComponent>(ent);
                if (audio) {
                    // free any old OpenAL handles — components should be in a reset state
                    if (audio->initialized) {
                        cleanupSource(*audio);
                    }
                    audio->initialized = false;
                    audio->source = 0;
                    audio->buffer = 0;
                }
            }
            needsReinit = false;
        }

        /* -----------------------------
           2. CLEANUP DELETED ENTITIES
           ----------------------------- */
        for (auto it = activeAudioEntities.begin(); it != activeAudioEntities.end(); ) {
            if (!entityManager.IsEntityValid(*it)) {
                // Entity was deleted, cleanup its audio component if any (safety)
                auto* audio = entityManager.GetComponent<AudioSourceComponent>(*it);
                if (audio && audio->initialized) {
                    cleanupSource(*audio);
                }
                it = activeAudioEntities.erase(it);
            }
            else {
                ++it;
            }
        }

        /* -----------------------------
           3. LISTENER UPDATE
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
           4. 3D SOURCES
           ----------------------------- */
        auto sourceQuery = entityManager.CreateQuery<AudioSourceComponent, Transform>();

        for (auto [ent, audio, t] : sourceQuery) {
            if (!audio->initialized) {
                initializeSource(*audio);
                activeAudioEntities.insert(ent);
            }

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

    // Track opened device name (most reliable way to know what we opened)
    std::string currentDeviceName;

    // Entities with active audio so we can cleanup/reinit on device change
    std::unordered_set<Entity> activeAudioEntities;
    bool needsReinit = false;

    // music state for restart
    std::string lastMusicFile;
    bool lastMusicLoop = true;

    // ----------------------------------------
    // initializeOpenAL: opens effective default device and stores name
    // ----------------------------------------
    bool initializeOpenAL() {
        // Determine effective default device (first enumerated device if possible)
        std::string effectiveDefault = GetEffectiveDefaultDevice(); // uses enumeration or fallback

        // Try to open chosen device (effectiveDefault may be empty, in which case we pass nullptr)
        const char* toOpen = effectiveDefault.empty() ? nullptr : effectiveDefault.c_str();

        device = alcOpenDevice(toOpen);
        if (!device) {
            Debug::Error("AudioSystem") << "Cannot open audio device (" << (toOpen ? toOpen : "nullptr") << ")\n";
            return false;
        }

        // store the name we attempted to open
        if (toOpen) currentDeviceName = toOpen;
        else {
            // best-effort: try to query what the implementation reports as device specifier
            const ALCchar* ds = alcGetString(device, ALC_DEVICE_SPECIFIER);
            currentDeviceName = ds ? std::string(reinterpret_cast<const char*>(ds)) : std::string("OpenAL-Unknown");
        }

        // Try create context with HRTF if available
        ALCint attrs[] = { ALC_HRTF_SOFT, ALC_TRUE, 0 };
        context = alcCreateContext(device, attrs);
        if (!context) {
            Debug::Warning("AudioSystem") << "HRTF not available on this device, creating normal context.\n";
            context = alcCreateContext(device, nullptr);
            if (!context) {
                Debug::Error("AudioSystem") << "Failed to create OpenAL context\n";
                alcCloseDevice(device);
                device = nullptr;
                return false;
            }
        }

        if (!alcMakeContextCurrent(context)) {
            Debug::Error("AudioSystem") << "alcMakeContextCurrent failed\n";
            alcDestroyContext(context);
            context = nullptr;
            alcCloseDevice(device);
            device = nullptr;
            return false;
        }

        Debug::Info("AudioSystem") << "OpenAL initialized on device: " << currentDeviceName << "\n";
        return true;
    }

    void shutdownOpenAL() {
        // delete music resources safely
        if (musicSource) {
            alSourceStop(musicSource);
            alDeleteSources(1, &musicSource);
            musicSource = 0;
        }
        if (musicBuffer) {
            alDeleteBuffers(1, &musicBuffer);
            musicBuffer = 0;
        }

        // cleanup active entity sources (best-effort)
        for (Entity ent : activeAudioEntities) {
            // we can't access entity manager here; components should be cleaned in Update when device changed
        }
        activeAudioEntities.clear();

        // teardown OpenAL context/device
        alcMakeContextCurrent(nullptr);
        if (context) {
            alcDestroyContext(context);
            context = nullptr;
        }
        if (device) {
            alcCloseDevice(device);
            device = nullptr;
        }

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

		//Debug::Info("AudioSystem") << "Listener position: (" << pos.x << ", " << pos.y << ", " << pos.z << ")\n";

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
        if (musicSource) {
            alDeleteSources(1, &musicSource);
            musicSource = 0;
        }
        alGenSources(1, &musicSource);
        alSourcef(musicSource, AL_GAIN, musicVolume);
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

    void cleanupSource(AudioSourceComponent& ac) {
        if (ac.initialized) {
            alSourceStop(ac.source);
            alDeleteSources(1, &ac.source);
            alDeleteBuffers(1, &ac.buffer);
            ac.initialized = false;
            ac.source = 0;
            ac.buffer = 0;
        }
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
        // Construct path to Content folder
        std::string contentPath = std::string("Content/") + file;

        unsigned int channels;
        unsigned int sampleRate;
        drwav_uint64 totalPCMFrames;
        float* pcmData = drwav_open_file_and_read_pcm_frames_f32(
            contentPath.c_str(), &channels, &sampleRate, &totalPCMFrames, nullptr);
        if (!pcmData) {
            Debug::Error("AudioSystem") << "Failed to load WAV: " << contentPath << "\n";
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
            static_cast<ALsizei>(totalPCMFrames * channels * sizeof(float)),
            sampleRate);
        drwav_free(pcmData, nullptr);
        formatOut = format;
        freqOut = sampleRate;
        return true;
    }
};

#endif
