#pragma once

#include <atomic>

// Latest network latency: written by OnlineClient's network thread, read by the render thread (debug overlay).
// Global because the overlay lives in IECSGameRenderer, which has no reference back to the OnlineClient.
namespace NetworkStats
{
    inline std::atomic<float> g_lastLatencyMs{ 0.0f };
    inline std::atomic<bool>  g_connected{ false };

    inline void SetLatencyMs(float ms) { g_lastLatencyMs.store(ms, std::memory_order_relaxed); }
    inline float GetLatencyMs() { return g_lastLatencyMs.load(std::memory_order_relaxed); }

    inline void SetConnected(bool v) { g_connected.store(v, std::memory_order_relaxed); }
    inline bool IsConnected() { return g_connected.load(std::memory_order_relaxed); }
}
