#ifndef AUDIO_MANAGER_HPP
#define AUDIO_MANAGER_HPP

#include "AudioSystem.hpp"
#include "ecs/ecs.hpp"
#include "Utils/Debug/Debug.hpp"

#include <thread>
#include <mutex>
#include <atomic>
#include <chrono>

const int AUDIO_TICKS_PER_SECOND = 30;
const int AUDIO_MS_PER_TICK = 1000 / AUDIO_TICKS_PER_SECOND;

class AudioManager {
private:
    AudioManager() = default;
    ~AudioManager() = default;
    AudioManager(const AudioManager&) = delete;
    AudioManager& operator=(const AudioManager&) = delete;

    static AudioSystem* audioSystem;
    static EntityManager* entityManager;
    static std::mutex            audioMutex;
    static std::thread           audioThread;
    static std::atomic<bool>     threadRunning;

	static std::string currentMusicFile;

    // Set by FlushEntities during client teardown so Update() doesn't re-init sources mid-cleanup (avoids ghost audio).
    static std::atomic<bool>     flushing;

public:
    static void Start() {
        std::lock_guard<std::mutex> lock(audioMutex);
        if (!threadRunning) {
            threadRunning = true;
            audioThread = std::thread([]() {
                audioSystem = new AudioSystem();
                Debug::Info("AudioManager") << "Audio system initialized on dedicated thread\n";
                audioLoop();
                delete audioSystem;
                audioSystem = nullptr;
                });
        }
    }

    static void Stop() {
        {
            std::lock_guard<std::mutex> lock(audioMutex);
            threadRunning = false;
        }
        if (audioThread.joinable()) {
            audioThread.join();
        }
    }

    static void SetEntityManager(EntityManager* em) {
        std::lock_guard<std::mutex> lock(audioMutex);
        entityManager = em;
    }

    // Call BEFORE CloseClient()/unloadBin(): releases every AL source held by the closing client's entities so they don't leak into the pool or keep playing as ghost audio.
    static void FlushEntities(EntityManager* em) {
        if (!em) return;

        std::lock_guard<std::mutex> lock(audioMutex);
        if (!audioSystem) return;

        // Block Update() from running while we tear down sources.
        flushing = true;

        auto sourceQuery = em->CreateQuery<AudioSourceComponent, Transform>();
        for (auto [ent, audio, t] : sourceQuery) {
            if (audio->initialized) {
                audioSystem->cleanupSourceAndUntrack(*audio, ent);
            }
        }

        // Detach the EM so the audio loop won't call Update() on a
        // destroyed EntityManager on its next tick.
        if (entityManager == em) {
            entityManager = nullptr;
        }

        flushing = false;

        Debug::Info("AudioManager") << "Flushed audio entities for closing client\n";
    }

    // Authoritative client-switch cleanup: operates directly on the AL source pool instead of the ECS, since FlushEntities alone can't reach entities when the EntityManager pointer isn't valid (e.g. OnlineClient moves gameLogic_ before teardown).
    static void StopAllSources() {
        std::lock_guard<std::mutex> lock(audioMutex);
        if (audioSystem) audioSystem->StopAllSources();
    }

    static void PlayMusic(const std::string& file, bool loop = true) {
        std::lock_guard<std::mutex> lock(audioMutex);

		if (currentMusicFile == file) return; // Avoid restarting the same music file

        if (audioSystem) audioSystem->PlayMusic(file, loop);
		currentMusicFile = file;
    }

    static void StopMusic() {
        std::lock_guard<std::mutex> lock(audioMutex);
        if (audioSystem) audioSystem->StopMusic();
		currentMusicFile.clear();
    }

    static void SetMusicVolume(float volume) {
        std::lock_guard<std::mutex> lock(audioMutex);
        if (audioSystem) audioSystem->SetMusicVolume(volume);
    }

    static void SetChannelVolume(AudioChannel channel, float volume) {
        std::lock_guard<std::mutex> lock(audioMutex);
        if (audioSystem) audioSystem->channels.SetVolume(channel, volume);
    }

    static float GetChannelVolume(AudioChannel channel) {
        std::lock_guard<std::mutex> lock(audioMutex);
        if (audioSystem) return audioSystem->channels.GetVolume(channel);
        return 1.0f;
    }

private:
    static void audioLoop() {
        auto nextTick = std::chrono::high_resolution_clock::now();
        auto lastTick = std::chrono::high_resolution_clock::now();

        int tickCount = 0;
        std::vector<long long> tickDurations;
        const size_t MAX_SAMPLES = 30;
        auto sampleStart = std::chrono::high_resolution_clock::now();

        while (threadRunning) {
            auto now = std::chrono::high_resolution_clock::now();
            float dt = std::chrono::duration<float>(now - lastTick).count();
            lastTick = now;

            // FIX: snapshot entityManager and call Update() under the same lock, so FlushEntities can't null it out mid-tick and cause ghost audio.
            {
                std::lock_guard<std::mutex> lock(audioMutex);

                // Skip this tick while FlushEntities is tearing down sources.
                if (!flushing && entityManager && audioSystem) {
                    std::vector<EventEntry> events;
                    audioSystem->Update(*entityManager, events, false, dt);
                }
            }

            tickCount++;
            if (tickCount == AUDIO_TICKS_PER_SECOND) {
                auto tickEnd = std::chrono::high_resolution_clock::now();
                auto duration = tickEnd - sampleStart;
                sampleStart = tickEnd;

                auto durationUs = std::chrono::duration_cast<std::chrono::microseconds>(duration).count();
                tickDurations.push_back(durationUs);
                if (tickDurations.size() > MAX_SAMPLES)
                    tickDurations.erase(tickDurations.begin());

                long long sum = 0;
                for (auto d : tickDurations) sum += d;
                double mean = static_cast<double>(sum) / tickDurations.size();

                double currentMs = (durationUs / 1000.0) / AUDIO_TICKS_PER_SECOND;
                double meanMs = (mean / 1000.0) / AUDIO_TICKS_PER_SECOND;

                tickCount = 0;
            }

            nextTick += std::chrono::milliseconds(AUDIO_MS_PER_TICK);
            std::this_thread::sleep_until(nextTick);
        }

        Debug::Info("AudioManager") << "Audio thread shutting down\n";
    }
};

AudioSystem* AudioManager::audioSystem = nullptr;
EntityManager* AudioManager::entityManager = nullptr;
std::mutex        AudioManager::audioMutex;
std::thread       AudioManager::audioThread;
std::atomic<bool> AudioManager::threadRunning{ false };
std::atomic<bool> AudioManager::flushing{ false };
std::string AudioManager::currentMusicFile = "";

#endif // AUDIO_MANAGER_HPP