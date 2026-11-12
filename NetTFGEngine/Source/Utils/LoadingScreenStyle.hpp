#pragma once

#include <mutex>
#include <string>

#include <glm/glm.hpp>

// How the engine's loading screen (OpenGL/LoadingScreen.hpp) looks and what it says. The game sets it once at startup
// (SetLoadingScreenStyle, any thread, before or after the window exists); the defaults are a neutral, English screen.
// Every text is UTF-8 and shown to the player as is: keep them generic, never asset or file names.
struct LoadingScreenStyle {
    // ---- Texts ----
    std::string title = "Loading";
    // After the load steps, while the scene's first frames build what it loads lazily (lighting, probes...).
    std::string warmUpLabel = "Preparing the scene";
    // Same phase for a scene whose first state comes from a server, until it arrives.
    std::string waitingForServerLabel = "Waiting for the server";
    // The engine's own load steps (IGameRenderer / IECSGameRenderer); a game's BuildECSLoadPlan labels its own steps.
    std::string sceneSetupLabel = "Setting up the scene";
    std::string sceneBuildLabel = "Building the scene";
    std::string renderSetupLabel = "Starting the renderer";
    std::string interfaceSetupLabel = "Preparing the interface";

    bool showPercent = true;
    bool animatedDots = true;   // "Label", "Label.", "Label.."... so a long step still looks alive
    bool showSweep = true;      // highlight moving back and forth along the bar, same purpose

    // ---- Look (RGBA, 0-1) ----
    glm::vec4 backgroundColor{ 0.03f, 0.03f, 0.035f, 1.0f };
    glm::vec4 titleColor{ 1.0f, 1.0f, 1.0f, 1.0f };
    glm::vec4 textColor{ 0.85f, 0.85f, 0.85f, 1.0f };
    glm::vec4 barBackColor{ 1.0f, 1.0f, 1.0f, 0.10f };
    glm::vec4 barFillColor{ 0.35f, 0.65f, 1.0f, 1.0f };
    glm::vec4 sweepColor{ 1.0f, 1.0f, 1.0f, 0.16f };

    // Sizes in the UI's 1920x1080 reference space (scaled to the window like the rest of the UI).
    float titleSize = 52.0f;
    float textSize = 28.0f;
    float barWidth = 900.0f;
    float barHeight = 14.0f;

    // TTF file; empty = the OS default UI font (LoadDefaultUIFont). Falls back to that if it can't be loaded.
    std::string fontPath;

    // A load shorter than this never shows the screen's content (no flash of text when e.g. a menu reloads).
    int showAfterMs = 150;
};

namespace LoadingScreenStyleDetail {
    inline std::mutex& Mutex() { static std::mutex m; return m; }
    inline LoadingScreenStyle& Current() { static LoadingScreenStyle s; return s; }
}

inline void SetLoadingScreenStyle(const LoadingScreenStyle& style) {
    std::lock_guard<std::mutex> lock(LoadingScreenStyleDetail::Mutex());
    LoadingScreenStyleDetail::Current() = style;
}

// A copy: the render thread reads it every frame while the game may set it from another thread.
inline LoadingScreenStyle GetLoadingScreenStyle() {
    std::lock_guard<std::mutex> lock(LoadingScreenStyleDetail::Mutex());
    return LoadingScreenStyleDetail::Current();
}
