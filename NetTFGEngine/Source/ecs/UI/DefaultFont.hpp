#pragma once

#include <filesystem>
#include <system_error>

#include "ecs/UI/UIRenderSystem.hpp"
#include "Utils/Debug/Debug.hpp"

// Loads the UI's "default" font into `uir` from the OS font folder; the first candidate that exists wins. A missing font
// leaves every UIText/WorldText invisible (no glyphs), which is what happened on Linux with the old hardcoded Windows
// path. Shared by every scene (IECSGameRenderer) and the loading screen.
inline bool LoadDefaultUIFont(UIRenderSystem* uir) {
    static const char* const candidates[] = {
#ifdef _WIN32
        "C:/Windows/Fonts/arial.ttf",
        "C:/Windows/Fonts/segoeui.ttf",
#else
        // Debian/Ubuntu, then Fedora/Arch layouts.
        "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
        "/usr/share/fonts/truetype/liberation2/LiberationSans-Regular.ttf",
        "/usr/share/fonts/liberation-sans/LiberationSans-Regular.ttf",
        "/usr/share/fonts/liberation/LiberationSans-Regular.ttf",
        "/usr/share/fonts/TTF/LiberationSans-Regular.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/dejavu-sans-fonts/DejaVuSans.ttf",
        "/usr/share/fonts/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/TTF/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf",
        "/usr/share/fonts/noto/NotoSans-Regular.ttf",
        "/usr/share/fonts/google-noto/NotoSans-Regular.ttf",
        "/usr/share/fonts/truetype/freefont/FreeSans.ttf",
        "/usr/share/fonts/gnu-free/FreeSans.ttf",
#endif
    };
    for (const char* path : candidates) {
        std::error_code ec;
        // Quiet: under Wine/Proton arial.ttf can "exist" and still not open, and the next candidate is fine.
        if (std::filesystem::exists(path, ec) && uir->LoadFont("default", path, 32, true)) return true;
    }
    Debug::Error("UI") << "No usable UI font found; text will not render. Install Liberation Sans or DejaVu Sans.\n";
    return false;
}
