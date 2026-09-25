#ifndef NETCODE_CLIENT_WINDOW_H
#define NETCODE_CLIENT_WINDOW_H
#include "netcode_common.hpp"
#include "OpenGL/OpenGLWindow.hpp"
#include "OpenGL/Mesh.hpp"
#include "OpenGL/Render pipeline/RenderSettings.hpp"
#include "Utils/Input.hpp"
#include "Utils/Debug/Debug.hpp"
#include <functional>
#include <future>
#include <algorithm>
#include <thread>
#include <chrono>

#if defined(_WIN32)
#include <windows.h>
#include <timeapi.h>
#pragma comment(lib, "winmm.lib")

// RAII timeBeginPeriod/timeEndPeriod. Windows' default ~15.6ms timer tick makes sleep_until/sleep_for wake late;
// 1ms resolution (plus renderLoop()'s spin-wait) gets sub-ms pacing instead of e.g. 240 FPS reaching only ~65.
class HighResTimerGuard {
public:
    HighResTimerGuard() { timeBeginPeriod(1); }
    ~HighResTimerGuard() { timeEndPeriod(1); }
    HighResTimerGuard(const HighResTimerGuard&) = delete;
    HighResTimerGuard& operator=(const HighResTimerGuard&) = delete;
};
#else
// No-op elsewhere: clock_nanosleep(CLOCK_MONOTONIC) on Linux is already
// precise without needing a system timer resolution bump.
class HighResTimerGuard {
public:
    HighResTimerGuard() = default;
    HighResTimerGuard(const HighResTimerGuard&) = delete;
    HighResTimerGuard& operator=(const HighResTimerGuard&) = delete;
};
#endif

// Target render FPS now lives in RenderSettings (RenderSettings::instance().getTargetFPS()),
// adjustable at runtime from the settings menu instead of being fixed at compile time.
inline int CurrentTargetFPS() { return std::max(1, RenderSettings::instance().getTargetFPS()); }

// Microsecond tick period: milliseconds are too coarse above ~60 FPS (144 and 165 both floor to 6ms, ~166 FPS).
// Microseconds keep the rounding error under ~0.05% across the whole FPS-limit dropdown.
inline long long CurrentTickPeriodUs() { return 1000000LL / CurrentTargetFPS(); }

class ClientWindow {

    std::atomic<bool> gRunning{ true };

    std::vector<long long> tickDurations_;
    const size_t MAX_SAMPLES = 30;

    int tickCount = 0;

    // Singleton window instance (shared across all ClientWindow instances)
    static OpenGLWindow* window;
    static std::mutex windowMutex;
    static std::thread renderThread;
    static std::vector<ClientWindow*> activeInstances;
    static bool threadRunning;

    // Tasks queued from other threads to run on the render thread, where the
    // GL context is current. Drained once per renderLoop iteration.
    static std::vector<std::function<void()>> pendingTasks;

    // Set at thread start; lets RunOnRenderThread detect reentrant calls from the render thread itself and run inline instead of deadlocking on its own queue.
    static std::thread::id renderThreadId;

    // Set by renderLoop() when the OS window is asked to close, without touching threadRunning/the GL context, so shutdown can release ECS GL resources before the context is destroyed.
    static std::atomic<bool> closeRequested;

    // Smoothed real (measured) frame rate of the render loop, refreshed every
    // iteration; read by the debug overlay. Separate from CurrentTargetFPS(),
    // which is the configured cap, not what's actually being achieved.
    static std::atomic<float> currentFps;

public:

    // Real, measured FPS (exponential moving average), not the configured target.
    static float GetCurrentFPS() { return currentFps.load(std::memory_order_relaxed); }

    std::mutex gStateMutex;
    GameStateBlob PreviousServerState;
    GameStateBlob CurrentServerState;
    GameStateBlob RenderState;
    GameStateBlob PreviousLocalState;
    GameStateBlob CurrentLocalState;
    std::chrono::steady_clock::time_point lastStateUpdate;
    std::chrono::steady_clock::time_point previousStateUpdate;
    std::chrono::steady_clock::time_point lastLocalUpdate;
    std::chrono::steady_clock::time_point previousLocalUpdate;
    std::function<void(GameStateBlob&, OpenGLWindow*)> renderInitCallback;
    std::function<void(GameStateBlob&, OpenGLWindow*)> renderCallback;
    std::function<void(const GameStateBlob&, const GameStateBlob&, const GameStateBlob&, const GameStateBlob&, GameStateBlob&, float, float)> interpolationCallback;

    bool needsInit;  // Track per-instance initialization

    ClientWindow(std::function<void(GameStateBlob&, OpenGLWindow*)> initCb,
        std::function<void(GameStateBlob&, OpenGLWindow*)> renderCb,
        std::function<void(const GameStateBlob&, const GameStateBlob&, const GameStateBlob&, const GameStateBlob&, GameStateBlob&, float, float)> interpolationCb)
        : renderInitCallback(initCb), renderCallback(renderCb), interpolationCallback(interpolationCb), needsInit(true)
    {
        lastStateUpdate = std::chrono::steady_clock::now();
        previousStateUpdate = std::chrono::steady_clock::now();
        lastLocalUpdate = std::chrono::steady_clock::now();
        previousLocalUpdate = std::chrono::steady_clock::now();

        PreviousServerState.frame = -1;
        CurrentServerState.frame = -1;
        PreviousLocalState.frame = -1;
        CurrentLocalState.frame = -1;
        RenderState.frame = -1;
    }

    ~ClientWindow() {
        deactivate();
    }

    static void startRenderThread(int width = 800, int height = 600, const std::string& title = "Client") {
        std::lock_guard<std::mutex> lock(windowMutex);
        if (!threadRunning) {
            threadRunning = true;
            closeRequested = false;
            renderThread = std::thread([width, height, title]() {
                renderThreadId = std::this_thread::get_id();

                // Scoped to the whole thread body so it releases even if
                // renderLoop() were to exit early.
                HighResTimerGuard highResTimer;

                window = new OpenGLWindow(width, height, title);
                Input::Init(window->getWindow());

                // Engine-level GL resources that don't belong to any scene: compile the default surface shader now, with the context just created, instead of on the first mesh.
                Mesh::InitDefaultMaterial();

                // Apply window mode and VSync saved from a previous session.
                window->setWindowMode(RenderSettings::instance().getWindowMode());
                window->setVSync(RenderSettings::instance().getVsyncEnabled());

                renderLoop();

                // While the GL context still exists (deleting the window destroys it).
                Mesh::ReleaseDefaultMaterial();

                delete window;
                window = nullptr;
                });
        }
    }

    static void stopRenderThread() {
        {
            std::lock_guard<std::mutex> lock(windowMutex);
            threadRunning = false;
            if (window) window->close();
        }
        if (renderThread.joinable()) {
            renderThread.join();
        }
    }

    void activate() {
        std::lock_guard<std::mutex> lock(windowMutex);

        auto it = std::find(activeInstances.begin(), activeInstances.end(), this);
        if (it == activeInstances.end()) {
            activeInstances.push_back(this);
            needsInit = true;
        }

        gRunning = true;
    }

    void deactivate() {
        std::lock_guard<std::mutex> lock(windowMutex);
        auto it = std::find(activeInstances.begin(), activeInstances.end(), this);
        if (it != activeInstances.end()) {
            activeInstances.erase(it);
        }
        gRunning = false;
    }

    void close() {
        gRunning = false;
        deactivate();
    }

    bool isRunning() const { return gRunning; }

    void setServerState(GameStateBlob state) {
        std::lock_guard<std::mutex> lock(gStateMutex);


        if (state.frame > CurrentServerState.frame) {
            PreviousServerState = CurrentServerState;
            previousStateUpdate = lastStateUpdate;
            CurrentServerState = state;
            lastStateUpdate = std::chrono::steady_clock::now();
        }
    }

    void setLocalState(GameStateBlob state) {
        std::lock_guard<std::mutex> lock(gStateMutex);

        // != rather than >: only the game thread calls this, in order, so a lower frame is a reconciliation rewinding the
        // prediction (currentFrame = lastConfirmed + framesAhead). Rejecting it froze the local player until the frame
        // passed the old maximum again, then snapped.
        if (state.frame != CurrentLocalState.frame) {
            PreviousLocalState = CurrentLocalState;
            previousLocalUpdate = lastLocalUpdate;
            CurrentLocalState = state;
            lastLocalUpdate = std::chrono::steady_clock::now();
        }
    }

    GameStateBlob getLocalState() {
        std::lock_guard<std::mutex> lock(gStateMutex);
        return CurrentLocalState;
    }

    GameStateBlob getServerState() {
        std::lock_guard<std::mutex> lock(gStateMutex);
        return CurrentServerState;
    }

    static bool isWindowThreadRunning() {
        std::lock_guard<std::mutex> lock(windowMutex);
        return threadRunning;
    }

    // True once the OS window close was requested (render thread/GL context still alive); NetTFG_Engine::Start() polls this to begin shutdown.
    static bool IsCloseRequested() {
        return closeRequested.load();
    }

    // Runs fn on the render thread (blocking) since the GL context is only current there; runs inline if already on it (avoids deadlocking on its own queue), and skips fn entirely if the render thread isn't running (shutdown, no safe context to fall back to).
    static void RunOnRenderThread(std::function<void()> fn) {
        if (std::this_thread::get_id() == renderThreadId) {
            fn();
            return;
        }

        if (!isWindowThreadRunning()) {
            return;
        }

        auto done = std::make_shared<std::promise<void>>();
        std::future<void> fut = done->get_future();

        {
            std::lock_guard<std::mutex> lock(windowMutex);
            pendingTasks.push_back([fn, done]() {
                fn();
                done->set_value();
                });
        }

        fut.wait();
    }

    static size_t getActiveInstanceCount() {
        std::lock_guard<std::mutex> lock(windowMutex);
        return activeInstances.size();
    }

    // Raw window access for render-thread callers only (e.g. a UIButton::onClick,
    // which runs from inside renderLoop() via renderCallback -> Render() -> world.Update()).
    // Not locked: window is only ever written on the render thread itself.
    static OpenGLWindow* GetWindow() { return window; }

private:
    static void renderLoop() {
        auto frameStart = std::chrono::high_resolution_clock::now();
        auto previousFrameStart = frameStart;

        int tickCount = 0;
        std::vector<long long> tickDurations;
        const size_t MAX_SAMPLES = 30;

        while (threadRunning) {
            frameStart = std::chrono::high_resolution_clock::now();

            // Real measured FPS for the debug overlay: EMA over actual
            // iteration time, not the configured target used elsewhere.
            {
                float frameMs = std::chrono::duration<float, std::milli>(frameStart - previousFrameStart).count();
                previousFrameStart = frameStart;
                if (frameMs > 0.01f) {
                    float instFps = 1000.0f / frameMs;
                    float prev = currentFps.load(std::memory_order_relaxed);
                    float smoothed = (prev <= 0.0f) ? instFps : (prev * 0.9f + instFps * 0.1f);
                    currentFps.store(smoothed, std::memory_order_relaxed);
                }
            }

            window->pollEvents();

            // Run cleanup queued from other threads; this is the only thread with the GL context current.
            std::vector<std::function<void()>> tasksToRun;
            {
                std::lock_guard<std::mutex> lock(windowMutex);
                tasksToRun.swap(pendingTasks);
            }
            for (auto& task : tasksToRun) {
                task();
            }

            std::vector<ClientWindow*> instances;
            {
                std::lock_guard<std::mutex> lock(windowMutex);
                instances = activeInstances;
            }

            // Iconified (Alt+Tab out of fullscreen): 0x0 framebuffer, nothing to present. The render callback still runs so
            // ECS/audio keep advancing (it skips draw systems when minimized); only the swap is skipped.
            const bool minimized = window->isMinimized();

            for (ClientWindow* instance : instances) {
                if (!instance || !instance->gRunning) continue;

                if (instance->needsInit && instance->renderInitCallback) {
                    instance->renderInitCallback(instance->RenderState, window);
                    instance->needsInit = false;
                }

                instance->gStateMutex.lock();

                // Both states start at frame=-1 until TickClient() completes once on the game thread. Interpolating before that
                // blends toward zeroed placeholders (e.g. a ship at (0,0)) and then snaps. Skip interpolation and rendering until
                // both have ticked; Init() still runs above so the scene exists.
                bool hasRealState = instance->CurrentLocalState.frame != -1
                    && instance->CurrentServerState.frame != -1;

                if (!hasRealState) {
                    instance->gStateMutex.unlock();
                    continue;
                }

                auto now = std::chrono::steady_clock::now();

                // Server: sweeps 0->1 over TICK_DURATION after each new state arrives.
                // factor=0: render at prevServer. factor=1: render at currServer.
                float serverInterpolationFactor = 0.0f;
                if (instance->CurrentServerState.frame != instance->PreviousServerState.frame) {
                    auto elapsed = now - instance->lastStateUpdate;
                    float elapsedMs = std::chrono::duration<float, std::milli>(elapsed).count();
                    serverInterpolationFactor = elapsedMs / (TICK_DURATION.count() / 1000.0f);
                    if (serverInterpolationFactor < 0.0f) serverInterpolationFactor = 0.0f;
                    if (serverInterpolationFactor > 1.0f) serverInterpolationFactor = 1.0f;
                }

                // Local: sweeps 0->1 over TICK_DURATION after each new predicted state.
                // factor=0: render at prevLocal. factor=1: render at currLocal.
                float localInterpolationFactor = 0.0f;
                if (instance->CurrentLocalState.frame != instance->PreviousLocalState.frame) {
                    auto elapsed = now - instance->lastLocalUpdate;
                    float elapsedMs = std::chrono::duration<float, std::milli>(elapsed).count();
                    localInterpolationFactor = elapsedMs / (TICK_DURATION.count() / 1000.0f);
                    if (localInterpolationFactor < 0.0f) localInterpolationFactor = 0.0f;
                    if (localInterpolationFactor > 1.0f) localInterpolationFactor = 1.0f;
                }

                if (instance->interpolationCallback) {
                    instance->interpolationCallback(
                        instance->PreviousServerState,
                        instance->CurrentServerState,
                        instance->PreviousLocalState,
                        instance->CurrentLocalState,
                        instance->RenderState,
                        serverInterpolationFactor,
                        localInterpolationFactor
                    );
                }

                GameStateBlob stateCopy = instance->RenderState;
                instance->gStateMutex.unlock();

                if (instance->renderCallback) {
                    instance->renderCallback(stateCopy, window);
                }
            }

            if (!minimized) window->swapBuffers();

            // Signal-only: NetTFG_Engine::Start() sees IsCloseRequested() and runs shutdown; stopRenderThread() sets threadRunning false once done.
            if (!closeRequested && window->shouldClose()) {
                closeRequested = true;
                Debug::Info("ClientWindow") << "Window close requested; waiting for engine shutdown to release GL resources\n";
            }

            tickCount++;
            if (tickCount == CurrentTargetFPS()) {
                tickCount = 0;

                auto frameEnd = std::chrono::high_resolution_clock::now();
                auto frameDurationUs =
                    std::chrono::duration_cast<std::chrono::microseconds>(frameEnd - frameStart).count();

                tickDurations.push_back(frameDurationUs);
                if (tickDurations.size() > MAX_SAMPLES)
                    tickDurations.erase(tickDurations.begin());

                long long sum = 0;
                for (auto d : tickDurations)
                    sum += d;

                double currentMs = frameDurationUs / 1000.0;
                double currentFps = 1000.0 / currentMs;
                double meanMs = (sum / static_cast<double>(tickDurations.size())) / 1000.0;
                double meanFps = 1000.0 / meanMs;

                Debug::Info("ClientWindow")
                    << "Frame: " << currentMs << " ms | "
                    << "Avg: " << meanMs << " ms | "
                    << "FPS: " << currentFps << " | "
                    << "Avg FPS: " << meanFps << " | "
                    << "Active: " << instances.size()
                    << "\n";
            }

            Input::Update();

            // Deadline from this frame's own start, not an accumulated total: a slow frame only costs itself. An accumulated
            // nextTick fell permanently behind "now" (sleep_until on a past time returns instantly), so the FPS limit
            // effectively vanished once VSync stopped masking it.
            auto frameDeadline = frameStart + std::chrono::microseconds(CurrentTickPeriodUs());

            // Hybrid wait: sleep_until() alone overshoots by the scheduler granularity (~11-12ms observed even with
            // timeBeginPeriod(1)), most of a 240 FPS budget. Sleep the bulk cheaply, then spin the last stretch so wake
            // time is bounded by this thread checking the clock.
            constexpr auto kSpinMargin = std::chrono::microseconds(2000);
            auto nowBeforeWait = std::chrono::high_resolution_clock::now();
            if (frameDeadline - nowBeforeWait > kSpinMargin) {
                std::this_thread::sleep_until(frameDeadline - kSpinMargin);
            }
            while (std::chrono::high_resolution_clock::now() < frameDeadline) {
                std::this_thread::yield();
            }
        }
    }
};

OpenGLWindow* ClientWindow::window = nullptr;
std::mutex ClientWindow::windowMutex;
std::thread ClientWindow::renderThread;
std::vector<ClientWindow*> ClientWindow::activeInstances;
bool ClientWindow::threadRunning = false;
std::vector<std::function<void()>> ClientWindow::pendingTasks;
std::atomic<bool> ClientWindow::closeRequested{ false };
std::thread::id ClientWindow::renderThreadId;
std::atomic<float> ClientWindow::currentFps{ 0.0f };

#endif //NETCODE_CLIENT_WINDOW_H