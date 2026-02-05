#ifndef NETCODE_CLIENT_WINDOW_H
#define NETCODE_CLIENT_WINDOW_H
#include "netcode_common.hpp"
#include "OpenGL/OpenGLWindow.hpp"
#include "OpenGL/Mesh.hpp"
#include "Utils/Input.hpp"
#include "Utils/Debug/Debug.hpp"

const int RENDER_TICKS_PER_SECOND = 144;
const int RENDER_MS_PER_TICK = 1000 / RENDER_TICKS_PER_SECOND;

class ClientWindow {

    std::atomic<bool> gRunning{ true };

    std::vector<long long> tickDurations_;
    const size_t MAX_SAMPLES = 30;

    int tickCount = 0;

    // Singleton window instance (shared across all ClientWindow instances)
    static OpenGLWindow* window;
    static std::mutex windowMutex;
    static std::thread renderThread;
    static std::vector<ClientWindow*> activeInstances;  // Changed: vector of active instances
    static bool threadRunning;

public:

    std::mutex gStateMutex;
    GameStateBlob PreviousServerState;
    GameStateBlob CurrentServerState;
    GameStateBlob RenderState;
    GameStateBlob PreviousLocalState;
    GameStateBlob CurrentLocalState;
    std::chrono::steady_clock::time_point lastStateUpdate;
    std::chrono::steady_clock::time_point lastLocalUpdate;
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
        lastLocalUpdate = std::chrono::steady_clock::now();

		PreviousServerState.frame = -1;
		CurrentServerState.frame = -1;
		PreviousLocalState.frame = -1;
		CurrentLocalState.frame = -1;
		RenderState.frame = -1;
    }

    ~ClientWindow() {
        deactivate();
    }

    // Initialize the window and start the persistent render thread
    static void startRenderThread(int width = 800, int height = 600, const std::string& title = "Client") {
        std::lock_guard<std::mutex> lock(windowMutex);
        if (!threadRunning) {
            threadRunning = true;
            renderThread = std::thread([width, height, title]() {
                window = new OpenGLWindow(width, height, title);
                Input::Init(window->getWindow());

                renderLoop();

                delete window;
                window = nullptr;
                });
        }
    }

    // Stop the render thread and destroy window
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

    // Activate this ClientWindow instance (add to active list)
    void activate() {
        std::lock_guard<std::mutex> lock(windowMutex);

        // Check if already active
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
            CurrentServerState = state;
            lastStateUpdate = std::chrono::steady_clock::now();
        }
    }

    void setLocalState(GameStateBlob state) {
        std::lock_guard<std::mutex> lock(gStateMutex);
        PreviousLocalState = CurrentLocalState;
        CurrentLocalState = state;
        lastLocalUpdate = std::chrono::steady_clock::now();
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

    static size_t getActiveInstanceCount() {
        std::lock_guard<std::mutex> lock(windowMutex);
        return activeInstances.size();
    }

private:
    // The persistent render loop running on the dedicated thread
    static void renderLoop() {
        auto nextTick = std::chrono::high_resolution_clock::now();
        auto sampleStart = std::chrono::high_resolution_clock::now();

        int tickCount = 0;
        std::vector<long long> tickDurations;
        const size_t MAX_SAMPLES = 30;

        while (threadRunning && !(window->shouldClose())) {
            sampleStart = std::chrono::high_resolution_clock::now();

            window->pollEvents();

            // Get snapshot of active instances
            std::vector<ClientWindow*> instances;
            {
                std::lock_guard<std::mutex> lock(windowMutex);
                instances = activeInstances;
            }

            // Process each active instance
            for (ClientWindow* instance : instances) {
                if (!instance || !instance->gRunning) continue;

                // Call init callback if needed
                if (instance->needsInit && instance->renderInitCallback) {
                    instance->renderInitCallback(instance->RenderState, window);
                    instance->needsInit = false;
                }

                instance->gStateMutex.lock();

                // Calculate interpolation factors
                auto now = std::chrono::high_resolution_clock::now();

                float serverInterpolationFactor = 0.0f;
                if (instance->CurrentServerState.frame != instance->PreviousServerState.frame) {
                    auto frameDelta = std::chrono::milliseconds(MS_PER_TICK);
                    auto elapsed = now - instance->lastStateUpdate;  // ✅ FIXED: Use lastStateUpdate
                    serverInterpolationFactor = std::chrono::duration<float, std::milli>(elapsed).count() / frameDelta.count();
                    if (serverInterpolationFactor > 1.0f) serverInterpolationFactor = 1.0f;
                }

                float localInterpolationFactor = 0.0f;
                if (instance->CurrentLocalState.frame != instance->PreviousLocalState.frame) {  // ✅ FIXED: Check local frames
                    auto frameDelta = std::chrono::milliseconds(MS_PER_TICK);
                    auto elapsed = now - instance->lastLocalUpdate;  // ✅ FIXED: Use lastLocalUpdate
                    localInterpolationFactor = std::chrono::duration<float, std::milli>(elapsed).count() / frameDelta.count();
                    if (localInterpolationFactor > 1.0f) localInterpolationFactor = 1.0f;
                }

                // Interpolate
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

                // Render
                if (instance->renderCallback) {
                    instance->renderCallback(stateCopy, window);
                }
            }

            window->swapBuffers();

            if (window->shouldClose()) {
                std::lock_guard<std::mutex> lock(windowMutex);
                threadRunning = false;
                for (auto* instance : activeInstances) {
                    if (instance) {
                        instance->gRunning = false;
                    }
                }
                break;
            }

            tickCount++;
            if (tickCount == RENDER_TICKS_PER_SECOND) {
                auto now = std::chrono::high_resolution_clock::now();
                auto duration = now - sampleStart;
                sampleStart = now;

                auto durationUs = std::chrono::duration_cast<std::chrono::microseconds>(duration).count();
                tickDurations.push_back(durationUs);
                if (tickDurations.size() > MAX_SAMPLES) tickDurations.erase(tickDurations.begin());

                long long sum = 0;
                for (auto d : tickDurations) sum += d;
                double mean = static_cast<double>(sum) / tickDurations.size();

                double currentMs = (durationUs / 1000.0) / RENDER_TICKS_PER_SECOND;
                double meanMs = (mean / 1000.0) / RENDER_TICKS_PER_SECOND;

                Debug::Info("ClientWindow") << "Current: " << currentMs << " ms | Mean: " << meanMs << " ms | Active: " << instances.size() << "\n";

                tickCount = 0;
            }

            Input::Update();

            nextTick += std::chrono::milliseconds(RENDER_MS_PER_TICK);
            std::this_thread::sleep_until(nextTick);
        }
    }
};

// Static member definitions
OpenGLWindow* ClientWindow::window = nullptr;
std::mutex ClientWindow::windowMutex;
std::thread ClientWindow::renderThread;
std::vector<ClientWindow*> ClientWindow::activeInstances;
bool ClientWindow::threadRunning = false;

#endif //NETCODE_CLIENT_WINDOW_H