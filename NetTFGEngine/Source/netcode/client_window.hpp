#ifndef NETCODE_CLIENT_WINDOW_H
#define NETCODE_CLIENT_WINDOW_H
#include "netcode_common.hpp"
#include "OpenGL/OpenGLWindow.hpp"
#include "OpenGL/Mesh.hpp"
#include "Utils/Input.hpp"

const int RENDER_TICKS_PER_SECOND = 144;
const int RENDER_MS_PER_TICK = 1000 / RENDER_TICKS_PER_SECOND;

class ClientWindow {
    
    std::atomic<bool> gRunning{true};

    std::vector<long long> tickDurations_;
    const size_t MAX_SAMPLES = 30;

    int tickCount = 0;
    

    public:

    std::mutex gStateMutex;
	GameStateBlob PreviousState;   // previous state for interpolation
	GameStateBlob CurrentState;    // last received state from server
    GameStateBlob RenderState;       // last predicted state to draw
    OpenGLWindow* window{nullptr};
    std::chrono::steady_clock::time_point lastStateUpdate;
    std::function<void(GameStateBlob&,OpenGLWindow*)> renderInitCallback;
    std::function<void(GameStateBlob&,OpenGLWindow*)> renderCallback;
    std::function<void(const GameStateBlob&, const GameStateBlob&, GameStateBlob&, float)> interpolationCallback;

    ClientWindow(std::function<void(GameStateBlob&,OpenGLWindow*)> initCb,
                 std::function<void(GameStateBlob&,OpenGLWindow*)> renderCb,
        std::function<void(const GameStateBlob&, const GameStateBlob&, GameStateBlob&, float)> interpolationCb)
		: renderInitCallback(initCb), renderCallback(renderCb), interpolationCallback(interpolationCb)
        {
		lastStateUpdate = std::chrono::steady_clock::now();
        }

    void close() {
        gRunning = false;
        if (window) window->close();
    }

    bool isRunning() const { return gRunning;}

    void setRenderState(GameStateBlob std) {
        gStateMutex.lock();
        PreviousState = CurrentState;
        CurrentState = std;
		lastStateUpdate = std::chrono::steady_clock::now();
        gStateMutex.unlock();
    }

    bool run() {
        window = new OpenGLWindow(800, 600, "Client");
        Input::Init(window->getWindow());
        if (renderInitCallback) {
            renderInitCallback(RenderState, window);
        }

        auto nextTick = std::chrono::high_resolution_clock::now();
        auto sampleStart = std::chrono::high_resolution_clock::now();

        while (!window->shouldClose()) {
            sampleStart = std::chrono::high_resolution_clock::now();

            window->pollEvents();
            Input::Update();

            gStateMutex.lock();
            // Calcula el factor de interpolación
            auto now = std::chrono::high_resolution_clock::now();
            float interpolationFactor = 0.0f;
            if (CurrentState.frame != PreviousState.frame) {
                auto frameDelta = std::chrono::milliseconds(RENDER_MS_PER_TICK);
                auto elapsed = now - lastStateUpdate;
                interpolationFactor = std::chrono::duration<float, std::milli>(elapsed).count() / frameDelta.count();
                if (interpolationFactor > 1.0f) interpolationFactor = 1.0f;
            }
            interpolationCallback(PreviousState, CurrentState, RenderState, interpolationFactor);
            GameStateBlob stateCopy = RenderState;
            gStateMutex.unlock();

            if (renderCallback) {
                renderCallback(stateCopy, window);
            }
            //window->render();
            window->swapBuffers();
            

            

            tickCount++;

            if (tickCount == RENDER_TICKS_PER_SECOND) {
                auto now = std::chrono::high_resolution_clock::now();
                auto duration = now - sampleStart;   // ✅ measure real elapsed time
                sampleStart = now;                   // ✅ reset for next second

                auto durationUs = std::chrono::duration_cast<std::chrono::microseconds>(duration).count();
                tickDurations_.push_back(durationUs);
                if (tickDurations_.size() > MAX_SAMPLES) tickDurations_.erase(tickDurations_.begin());

                long long sum = 0;
                for (auto d : tickDurations_) sum += d;
                double mean = static_cast<double>(sum) / tickDurations_.size();

                double currentMs = (durationUs / 1000.0) / RENDER_TICKS_PER_SECOND;
                double meanMs    = (mean       / 1000.0) / RENDER_TICKS_PER_SECOND;

                std::cout << "Current: " << currentMs << " ms | Mean: " << meanMs << " ms\n";

                tickCount = 0;
            }

            nextTick += std::chrono::milliseconds(RENDER_MS_PER_TICK);
            std::this_thread::sleep_until(nextTick);
        }
        gRunning = false;
        // Clean up window owned by this thread
        if (window) {
            delete window;
            window = nullptr;
        }
        return true;
    }
};
#endif //NETCODE_CLIENT_WINDOW_H