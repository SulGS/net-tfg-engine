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

    // Set to true by FlushEntities while a client is being torn down.
    // Prevents Update() from re-initializing sources whose components were
    // just reset to initialized=false as part of the cleanup, which would
    // cause ghostly audio from the dying client on the very next tick.
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

    // -----------------------------------------------------------------------
    // FlushEntities — call BEFORE CloseClient() / unloadBin() on a client.
    //
    // Problem this solves:
    //   The audio thread runs independently. When a client is torn down its
    //   AudioSourceComponents still hold live AL source handles. Without an
    //   explicit flush those handles leak into the pool, their AssetManager
    //   buffer refs are never decremented, and the audio thread may call
    //   Update() on a dangling EntityManager pointer — all producing the
    //   "ghostly sound from dead client" symptom.
    //
    // What this does, atomically under the audio mutex:
    //   1. Sets flushing=true so the audio loop skips Update() for this tick,
    //      preventing it from re-initializing sources we're about to clean up.
    //   2. For every initialized AudioSourceComponent in the closing client's
    //      ECS: stops the AL source, returns it to the pool, drops the
    //      AssetManager buffer ref-count, resets the component fields, and
    //      removes the entity from activeAudioEntities.
    //   3. Clears the stored EntityManager pointer so the audio loop will not
    //      dereference it after this call returns.
    //   4. Clears flushing so the next client's audio resumes normally.
    //
    // The entire operation holds the mutex, so the audio thread is guaranteed
    // to be idle (sleeping between ticks) for the full duration of the flush.
    // -----------------------------------------------------------------------
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

    // -----------------------------------------------------------------------
    // StopAllSources — the guaranteed client-switch cleanup path.
    //
    // Why this exists instead of relying solely on FlushEntities:
    //   FlushEntities walks the ECS to find AudioSourceComponents, but that
    //   pointer is not always valid.  For OnlineClient, gameLogic_ is moved
    //   into ClientPredictionNetcode during SetupClient, so GetEntityManager()
    //   returns nullptr.  For any client, SetEntityManager may never have been
    //   called.  In both cases FlushEntities returns immediately having done
    //   nothing, and AL sources keep playing in the driver.
    //
    //   StopAllSources bypasses the ECS entirely and operates directly on the
    //   AL source pool.  It stops and detaches every slot that is in-use or
    //   audible, and clears the activeAudioEntities tracking set.  This is the
    //   authoritative cleanup; FlushEntities is best-effort on top of it.
    // -----------------------------------------------------------------------
    static void StopAllSources() {
        std::lock_guard<std::mutex> lock(audioMutex);
        if (audioSystem) audioSystem->StopAllSources();
    }

    // Music control
    static void PlayMusic(const std::string& file, bool loop = true) {
        std::lock_guard<std::mutex> lock(audioMutex);
        if (audioSystem) audioSystem->PlayMusic(file, loop);
    }

    static void StopMusic() {
        std::lock_guard<std::mutex> lock(audioMutex);
        if (audioSystem) audioSystem->StopMusic();
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

            // ---------------------------------------------------------------
            // FIX: snapshot + Update() are now a single critical section.
            //
            // Previously the loop did: lock → snapshot em → unlock → re-lock
            // → Update(*em). The gap between the two locks meant FlushEntities
            // could set entityManager=nullptr between them, but the stale local
            // `em` copy was already taken — so Update() still ran on the dying
            // client's ECS, re-initializing and re-playing sources that had
            // just been cleaned up (the "ghostly sound" bug).
            //
            // Now the snapshot and the Update() call share one lock, so if
            // FlushEntities holds the mutex first, entityManager is already
            // nullptr when we read it, and we skip Update() entirely.
            // If we hold the mutex first, FlushEntities will block until
            // Update() finishes, then clear entityManager safely.
            // ---------------------------------------------------------------
            {
                std::lock_guard<std::mutex> lock(audioMutex);

                // Skip this tick while FlushEntities is tearing down sources.
                // flushing is set/cleared inside the same mutex, so this check
                // is redundant given the single lock, but kept as a clear
                // statement of intent and defence against future refactors.
                if (!flushing && entityManager && audioSystem) {
                    std::vector<EventEntry> events;
                    audioSystem->Update(*entityManager, events, false, dt);
                }
            }

            // Performance monitoring
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

                //Debug::Info("AudioManager") << "Current: " << currentMs << " ms | Mean: " << meanMs << " ms\n";

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
std::mutex        AudioManager::audioMutex;
std::thread       AudioManager::audioThread;
std::atomic<bool> AudioManager::threadRunning{ false };
std::atomic<bool> AudioManager::flushing{ false };

#endif // AUDIO_MANAGER_HPP