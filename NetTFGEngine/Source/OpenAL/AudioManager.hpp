#ifndef AUDIO_MANAGER_HPP
#define AUDIO_MANAGER_HPP

#include "AudioSystem.hpp"
#include "ecs/ecs.hpp"
#include "Utils/Debug/Debug.hpp"

#include <thread>
#include <mutex>
#include <atomic>
#include <chrono>

const int AUDIO_TICKS_PER_SECOND = 60;
const int AUDIO_MS_PER_TICK = 1000 / AUDIO_TICKS_PER_SECOND;

class AudioManager {
private:
    // Private constructor for singleton
    AudioManager() = default;
    ~AudioManager() = default;

    // Delete copy/move
    AudioManager(const AudioManager&) = delete;
    AudioManager& operator=(const AudioManager&) = delete;

    // Singleton audio system instance
    static AudioSystem* audioSystem;
    static EntityManager* entityManager;
    static std::mutex audioMutex;
    static std::thread audioThread;
    static std::atomic<bool> threadRunning;

public:
    // Start the audio thread
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

    // Stop the audio thread
    static void Stop() {
        {
            std::lock_guard<std::mutex> lock(audioMutex);
            threadRunning = false;
        }
        if (audioThread.joinable()) {
            audioThread.join();
        }
    }

    // Set the entity manager to update
    static void SetEntityManager(EntityManager* em) {
        std::lock_guard<std::mutex> lock(audioMutex);
        entityManager = em;
    }

    // Music control
    static void PlayMusic(const std::string& file, bool loop = true) {
        std::lock_guard<std::mutex> lock(audioMutex);
        if (audioSystem) {
            audioSystem->PlayMusic(file, loop);
        }
    }

    static void StopMusic() {
        std::lock_guard<std::mutex> lock(audioMutex);
        if (audioSystem) {
            audioSystem->StopMusic();
        }
    }

    static void SetMusicVolume(float volume) {
        std::lock_guard<std::mutex> lock(audioMutex);
        if (audioSystem) {
            audioSystem->SetMusicVolume(volume);
        }
    }

    // Channel volume control
    static void SetChannelVolume(AudioChannel channel, float volume) {
        std::lock_guard<std::mutex> lock(audioMutex);
        if (audioSystem) {
            audioSystem->channels.SetVolume(channel, volume);
        }
    }

    static float GetChannelVolume(AudioChannel channel) {
        std::lock_guard<std::mutex> lock(audioMutex);
        if (audioSystem) {
            return audioSystem->channels.GetVolume(channel);
        }
        return 1.0f;
    }


private:
    // The persistent audio loop
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

            // Lock and get current state
            audioMutex.lock();
            EntityManager* em = entityManager;
            std::vector<EventEntry> events;
            audioMutex.unlock();

            // Update audio system if we have an entity manager
            if (em && audioSystem) {
                std::lock_guard<std::mutex> lock(audioMutex);
                audioSystem->Update(*em, events, false, dt);
            }

            // Performance monitoring
            tickCount++;
            if (tickCount == AUDIO_TICKS_PER_SECOND) {
                auto tickEnd = std::chrono::high_resolution_clock::now();
                auto duration = tickEnd - sampleStart;
                sampleStart = tickEnd;

                auto durationUs = std::chrono::duration_cast<std::chrono::microseconds>(duration).count();
                tickDurations.push_back(durationUs);
                if (tickDurations.size() > MAX_SAMPLES) {
                    tickDurations.erase(tickDurations.begin());
                }

                long long sum = 0;
                for (auto d : tickDurations) sum += d;
                double mean = static_cast<double>(sum) / tickDurations.size();

                double currentMs = (durationUs / 1000.0) / AUDIO_TICKS_PER_SECOND;
                double meanMs = (mean / 1000.0) / AUDIO_TICKS_PER_SECOND;

                Debug::Info("AudioManager") << "Current: " << currentMs << " ms | Mean: " << meanMs << " ms\n";

                tickCount = 0;
            }

            nextTick += std::chrono::milliseconds(AUDIO_MS_PER_TICK);
            std::this_thread::sleep_until(nextTick);
        }

        Debug::Info("AudioManager") << "Audio thread shutting down\n";
    }
};

// Static member definitions
AudioSystem* AudioManager::audioSystem = nullptr;
EntityManager* AudioManager::entityManager = nullptr;
std::mutex AudioManager::audioMutex;
std::thread AudioManager::audioThread;
std::atomic<bool> AudioManager::threadRunning{ false };

#endif // AUDIO_MANAGER_HPP