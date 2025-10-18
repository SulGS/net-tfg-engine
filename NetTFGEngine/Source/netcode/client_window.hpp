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
    GameStateBlob RenderState;       // last predicted state to draw
    OpenGLWindow* window{nullptr};
    std::function<void(GameStateBlob&,OpenGLWindow*)> renderInitCallback;
    std::function<void(GameStateBlob&,OpenGLWindow*)> renderCallback;

    ClientWindow(std::function<void(GameStateBlob&,OpenGLWindow*)> initCb,
                 std::function<void(GameStateBlob&,OpenGLWindow*)> renderCb)
        : renderInitCallback(initCb), renderCallback(renderCb) 
        {
            
        }

    void close() {
        gRunning = false;
        if (window) window->close();
    }

    bool isRunning() const { return gRunning;}

    void setRenderState(GameStateBlob std) {
        gStateMutex.lock();
        RenderState = std;
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
            gStateMutex.lock();
            GameStateBlob stateCopy = RenderState;
            gStateMutex.unlock();

            window->pollEvents();
            Input::Update();
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