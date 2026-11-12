#pragma once

#include <string>

// What the GPU running the GL context is, for the defaults that depend on it (shadow memory caps, the first-run
// quality preset). Needs the GL context current (render thread); every answer is computed once and cached.
// Implemented in Render pipeline/RenderSystem_Init.cpp, which already holds the platform queries (DXGI on Windows).
namespace GPUInfo
{
    // Memory the GPU can use for its resources, in bytes; 0 if unknown. Dedicated VRAM; an integrated GPU (tiny
    // dedicated carve-out) also counts half of the system memory it shares.
    long long VideoMemoryBytes();

    // Best guess: integrated GPU (shares system memory). Windows asks DXGI; elsewhere it goes by the renderer name.
    bool IsIntegrated();

    // GL_RENDERER, e.g. "AMD Radeon(TM) Graphics" or "Mesa Intel(R) Graphics (RPL-S)".
    std::string RendererName();
}
