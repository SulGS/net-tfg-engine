#ifndef NETCODE_CLIENT_WINDOW_H
#define NETCODE_CLIENT_WINDOW_H
#include "netcode_common.hpp"
#include "OpenGL/OpenGLWindow.hpp"
#include "OpenGL/Mesh.hpp"
#include "OpenGL/Render pipeline/RenderSettings.hpp"
#include "OpenGL/GPUInfo.hpp"
#include "OpenGL/LoadingScreen.hpp"
#include "Utils/LoadPlan.hpp"
#include "Utils/Input.hpp"
#include "Utils/InputMap.hpp"
#include "Utils/Debug/Debug.hpp"
#include <functional>
#include <future>
#include <algorithm>
#include <thread>
#include <chrono>
#include <iomanip>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX // see netcode_common.hpp
#endif
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
// With "no limit" (0) this returns a nominal 240 for the code that only needs a rough frame rate (first-frame dt,
// counters); the pacer and the FPS log check FPSUnlimited() instead.
inline bool FPSUnlimited() { return RenderSettings::instance().isFPSUnlimited(); }
inline int CurrentTargetFPS() { return FPSUnlimited() ? 240 : std::max(1, RenderSettings::instance().getTargetFPS()); }

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

    // Drawn over the scenes while one loads (render thread only; created on first use, released before the context).
    static std::unique_ptr<LoadingScreen> loadingScreen;

    // Load plan steps run each frame until this much time is spent (a single step can't be cut, so it may overrun).
    static constexpr std::chrono::microseconds kLoadSliceBudget{ 12000 };
    // Share of the bar for the load plan; the rest is the warm-up (first frames: lazily loaded lighting, server state).
    static constexpr float kPlanProgressShare = 0.85f;
    // The warm-up screen gives up after this long, so a scene that never renders smoothly isn't hidden forever.
    static constexpr std::chrono::seconds kMaxWarmUp{ 20 };
    // One frame this long kept the window from pumping its events: logged (Windows calls it "No responde" at ~5 s).
    static constexpr double kBlockedFrameWarnMs = 1000.0;

    // Per-instance load state (render thread only).
    LoadPlan loadPlan;
    bool loading = false;            // loadPlan still has steps to run
    bool loadScreenShown = false;    // this load lasted long enough to show the loading screen
    bool warmedUp = false;           // IsLevelLoaded() was reached once after the load (latched: later hitches don't count)
    bool lastHadRealState = false;   // both states had ticked at the last frame
    std::chrono::steady_clock::time_point loadStart;
    std::chrono::steady_clock::time_point loadEnd;

    void BeginLoad(OpenGLWindow* win) {
        needsInit = false;
        loadPlan = LoadPlan{};
        if (buildLoadPlanCallback) buildLoadPlanCallback(loadPlan, RenderState, win);
        loading = !loadPlan.Done();
        loadScreenShown = false;
        warmedUp = false;
        lastHadRealState = false;
        loadStart = loadEnd = std::chrono::steady_clock::now();
        smoothFramesSinceInit.store(0);
    }

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
    // Fills the plan that sets the scene up (IGameRenderer::BuildLoadPlan); renderLoop() runs it a slice per frame.
    std::function<void(LoadPlan&, const GameStateBlob&, OpenGLWindow*)> buildLoadPlanCallback;
    std::function<void(GameStateBlob&, OpenGLWindow*)> renderCallback;
    std::function<void(const GameStateBlob&, const GameStateBlob&, const GameStateBlob&, const GameStateBlob&, GameStateBlob&, float, float)> interpolationCallback;

    bool needsInit;  // Track per-instance initialization

    // Its first state comes from a server (OnlineClient): the loading screen says so while waiting for it.
    bool waitsForServer = false;

    // Consecutive rendered frames, since Init, that took less than LOADED_FRAME_MS: lazily loaded assets (sky, probes,
    // spritesheets...) still stall the first frames after Init, so "Init returned" alone isn't "loaded".
    static constexpr float LOADED_FRAME_MS = 150.0f;
    static constexpr int LOADED_SMOOTH_FRAMES = 20;
    std::atomic<int> smoothFramesSinceInit{ 0 };

    // True once the scene is loaded and renders smoothly (any thread).
    bool IsLevelLoaded() const { return smoothFramesSinceInit.load() >= LOADED_SMOOTH_FRAMES; }

    ClientWindow(std::function<void(LoadPlan&, const GameStateBlob&, OpenGLWindow*)> loadPlanCb,
        std::function<void(GameStateBlob&, OpenGLWindow*)> renderCb,
        std::function<void(const GameStateBlob&, const GameStateBlob&, const GameStateBlob&, const GameStateBlob&, GameStateBlob&, float, float)> interpolationCb)
        : buildLoadPlanCallback(loadPlanCb), renderCallback(renderCb), interpolationCallback(interpolationCb), needsInit(true)
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
                Debug::SetThreadName("Render");

                // Scoped to the whole thread body so it releases even if
                // renderLoop() were to exit early.
                HighResTimerGuard highResTimer;

                // The saved window resolution wins over the size the game asked for; that one is only the
                // fallback when there is no valid setting.
                const RenderSettings& rs = RenderSettings::instance();
                const int windowW = rs.getWindowWidth() > 0 ? rs.getWindowWidth() : width;
                const int windowH = rs.getWindowHeight() > 0 ? rs.getWindowHeight() : height;

                window = new OpenGLWindow(windowW, windowH, title);
                window->setWindowedSize(windowW, windowH); // clamps to the monitor and centres
                Input::Init(window->getWindow());

                // Needs the context just created; before any scene's RenderSystem reads the settings.
                {
                    int fbW = 0, fbH = 0;
                    glfwGetFramebufferSize(window->getWindow(), &fbW, &fbH);
                    if (rs.getWindowMode() != WindowMode::Windowed) {
                        if (const GLFWvidmode* mode = glfwGetVideoMode(glfwGetPrimaryMonitor())) fbH = mode->height;
                    }
                    RenderSettings::instance().autoDetectIfFirstRun(GPUInfo::RendererName(), GPUInfo::IsIntegrated(),
                        GPUInfo::VideoMemoryBytes(), fbH);
                }

                // Engine-level GL resources that don't belong to any scene: compile the default surface shader now, with the context just created, instead of on the first mesh.
                Mesh::InitDefaultMaterial();

                // Apply window mode and VSync saved from a previous session.
                window->setWindowMode(rs.getWindowMode());
                window->setVSync(rs.getVsyncEnabled());

                renderLoop();

                // While the GL context still exists (deleting the window destroys it).
                if (loadingScreen) loadingScreen->Release();
                loadingScreen.reset();
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
            // First real state: Previous would otherwise be the never-written placeholder (frame -1), and renderLoop()
            // would interpolate from it. Seeding it with the same state renders that first state as-is.
            PreviousServerState = (CurrentServerState.frame == -1) ? state : CurrentServerState;
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
            // See setServerState(): never let the frame -1 placeholder become the interpolation source.
            PreviousLocalState = (CurrentLocalState.frame == -1) ? state : CurrentLocalState;
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
    // Loading screen for the first instance that is loading: during its plan, then over its warm-up frames (lighting
    // built on the first frames, waiting for the first server state) if the screen was already up. True if drawn.
    static bool DrawLoadingScreen(const std::vector<ClientWindow*>& instances, bool anySceneDrawn, bool minimized) {
        using Clock = std::chrono::steady_clock;
        static Clock::time_point lastDraw = Clock::now();
        const auto now = Clock::now();
        const float dt = std::chrono::duration<float>(now - lastDraw).count();
        lastDraw = now;

        const LoadingScreenStyle style = GetLoadingScreenStyle();
        // A load shorter than this never shows the screen's text and bar (no flash when a menu reloads).
        const auto showAfter = std::chrono::milliseconds(std::max(0, style.showAfterMs));

        for (ClientWindow* instance : instances) {
            if (!instance || !instance->gRunning) continue;

            float progress = 0.0f;
            std::string label;
            if (instance->loading) {
                if (!instance->loadScreenShown && now - instance->loadStart >= showAfter) {
                    instance->loadScreenShown = true;
                    if (!loadingScreen) loadingScreen = std::make_unique<LoadingScreen>();
                    loadingScreen->Begin();
                }
                progress = instance->loadPlan.Progress() * kPlanProgressShare;
                label = instance->loadPlan.CurrentLabel();
            }
            else if (instance->loadScreenShown && !instance->warmedUp && now - instance->loadEnd < kMaxWarmUp) {
                const float warm = std::min(1.0f,
                    float(instance->smoothFramesSinceInit.load()) / float(LOADED_SMOOTH_FRAMES));
                progress = kPlanProgressShare + (1.0f - kPlanProgressShare) * (instance->lastHadRealState ? warm : 0.0f);
                label = (instance->lastHadRealState || !instance->waitsForServer)
                    ? style.warmUpLabel : style.waitingForServerLabel;
            }
            else {
                continue;
            }

            // Before the delay only a blank frame, and only when no scene drew one (a short load shows nothing).
            if (!instance->loadScreenShown && anySceneDrawn) return false;
            if (minimized) return true;
            if (!loadingScreen) loadingScreen = std::make_unique<LoadingScreen>();
            loadingScreen->Draw(window, progress, label, instance->loadScreenShown, dt);
            return true;
        }
        return false;
    }

    static void renderLoop() {
        auto frameStart = std::chrono::high_resolution_clock::now();
        auto previousFrameStart = frameStart;

        // ClientWindow log line, about once a second. FPS comes from the real frame-to-frame interval (what the player
        // sees), so it never goes over the FPS limit; the old figure was 1000 / (time spent rendering one frame), which
        // ignored the limiter's wait and read "2000 FPS" in menus. That render time is still logged, as "Work": the
        // headroom left under the limit.
        auto statsStart = frameStart;
        int statsFrames = 0;            // frame intervals closed since statsStart
        double statsWorstMs = 0.0;      // longest interval in the window (hitches)
        double statsWorkMs = 0.0;       // summed render time (frame start -> after swap, before the wait)
        std::vector<std::pair<int, double>> statsHistory; // (frames, seconds) of the last windows, for "Avg"
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
                    ++statsFrames;
                    statsWorstMs = std::max(statsWorstMs, static_cast<double>(frameMs));
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

            bool anySceneDrawn = false;   // some scene drew this frame (the loading screen's blank phase leaves it be)

            for (ClientWindow* instance : instances) {
                if (!instance || !instance->gRunning) continue;

                if (instance->needsInit) instance->BeginLoad(window);

                // The scene's setup, a slice per frame: this loop keeps pumping window events and drawing the loading
                // screen in between. The scene isn't rendered until its whole plan has run; the frame its last step
                // runs isn't rendered either, so that step and the first (heaviest) scene frame don't add up.
                if (instance->loading) {
                    instance->loadPlan.Run(kLoadSliceBudget);
                    if (instance->loadPlan.Done()) {
                        instance->loading = false;
                        instance->loadEnd = std::chrono::steady_clock::now();
                        instance->smoothFramesSinceInit.store(0);
                    }
                    continue;
                }

                instance->gStateMutex.lock();

                // Both states start at frame=-1 until TickClient() completes once on the game thread. Interpolating before that
                // blends toward zeroed placeholders (e.g. a ship at (0,0)) and then snaps. Skip interpolation and rendering until
                // both have ticked; the load plan still runs above so the scene exists.
                bool hasRealState = instance->CurrentLocalState.frame != -1
                    && instance->CurrentServerState.frame != -1;
                instance->lastHadRealState = hasRealState;

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
                    anySceneDrawn = true;
                }

                // This frame so far (tasks + every instance up to this one): a lazy asset load shows up here.
                const float workMs = std::chrono::duration<float, std::milli>(
                    std::chrono::high_resolution_clock::now() - frameStart).count();
                if (workMs < LOADED_FRAME_MS) {
                    if (instance->smoothFramesSinceInit.load() < LOADED_SMOOTH_FRAMES)
                        instance->smoothFramesSinceInit.fetch_add(1);
                }
                else {
                    instance->smoothFramesSinceInit.store(0);
                }

                if (!instance->warmedUp && instance->IsLevelLoaded()) {
                    instance->warmedUp = true;
                    const auto now = std::chrono::steady_clock::now();
                    Debug::Info("Loading") << "Scene ready "
                        << std::chrono::duration<double, std::milli>(now - instance->loadStart).count() << " ms after its load started ("
                        << std::chrono::duration<double, std::milli>(now - instance->loadEnd).count() << " ms of warm-up frames)\n";
                }
            }

            const bool loadOnScreen = DrawLoadingScreen(instances, anySceneDrawn, minimized);

            if (!minimized) window->swapBuffers();

            // Whatever this frame did, the window's events waited for all of it.
            {
                const double frameWorkMs = std::chrono::duration<double, std::milli>(
                    std::chrono::high_resolution_clock::now() - frameStart).count();
                if (frameWorkMs >= kBlockedFrameWarnMs)
                    Debug::Warning("ClientWindow") << "One frame took " << frameWorkMs << " ms without pumping window events"
                        << (loadOnScreen ? " (loading)" : "") << "\n";
            }

            // Signal-only: NetTFG_Engine::Start() sees IsCloseRequested() and runs shutdown; stopRenderThread() sets threadRunning false once done.
            if (!closeRequested && window->shouldClose()) {
                closeRequested = true;
                Debug::Info("ClientWindow") << "Window close requested; waiting for engine shutdown to release GL resources\n";
            }

            // Time-based window (not every N frames): an N tied to the FPS limit stopped matching when the limit was
            // lowered mid-session, and a frame-count window at 30 FPS was several seconds long.
            const double statsWindowS = std::chrono::duration<double>(frameStart - statsStart).count();
            if (statsFrames > 0 && statsWindowS >= 1.0) {
                // No limit: nothing to clamp to (logged as "Limit: 0").
                const double limit = FPSUnlimited() ? 1e9 : static_cast<double>(CurrentTargetFPS());
                statsHistory.emplace_back(statsFrames, statsWindowS);
                if (statsHistory.size() > MAX_SAMPLES)
                    statsHistory.erase(statsHistory.begin());
                int histFrames = 0;
                double histSeconds = 0.0;
                for (const auto& [frames, seconds] : statsHistory) {
                    histFrames += frames;
                    histSeconds += seconds;
                }
                // Pacing jitter can put the measured rate a hair over the limit: never report more than the limit.
                const double fps = std::min(statsFrames / statsWindowS, limit);
                const double avgFps = std::min(histFrames / histSeconds, limit);

                Debug::Info("ClientWindow") << std::fixed << std::setprecision(2)
                    << "Frame: " << 1000.0 / fps << " ms | "
                    << "Avg: " << 1000.0 / avgFps << " ms | "
                    << "FPS: " << fps << " | "
                    << "Avg FPS: " << avgFps << " | "
                    << "Worst: " << statsWorstMs << " ms | "
                    << "Work: " << statsWorkMs / statsFrames << " ms | "
                    << "Limit: " << (FPSUnlimited() ? 0 : static_cast<int>(limit)) << " | "
                    << "Active: " << instances.size()
                    << "\n";

                statsStart = frameStart;
                statsFrames = 0;
                statsWorstMs = 0.0;
                statsWorkMs = 0.0;
            }

            // After the window check: this frame's interval closes at the next frame start, so it belongs to the next window.
            statsWorkMs += std::chrono::duration<double, std::milli>(
                std::chrono::high_resolution_clock::now() - frameStart).count();

            Input::Update();
            // Actions from the raw state just aged, and the snapshot the game tick reads (ConsumeTickInput).
            InputMap::Get().Update();

            // Deadline from this frame's own start, not an accumulated total: a slow frame only costs itself. An accumulated
            // nextTick fell permanently behind "now" (sleep_until on a past time returns instantly), so the FPS limit
            // effectively vanished once VSync stopped masking it.
            // No FPS limit: straight to the next frame.
            if (FPSUnlimited()) continue;
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
std::unique_ptr<LoadingScreen> ClientWindow::loadingScreen;

#endif //NETCODE_CLIENT_WINDOW_H