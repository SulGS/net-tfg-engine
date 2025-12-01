#include "Utils/Debug/Debug.hpp"

class InputDelayCalculator {
public:
    InputDelayCalculator()
        : m_lastRttMs(0),
        m_lastLatencyMs(0.0f),
        m_lastInputDelayFrames(0)
    {
    }

    static uint32_t GetTimestampMs() {
        auto now = std::chrono::high_resolution_clock::now();
        auto ms = std::chrono::time_point_cast<std::chrono::milliseconds>(now);
        uint64_t totalMs = ms.time_since_epoch().count();
        return static_cast<uint32_t>(totalMs & 0xFFFFFFFF);
    }

    void UpdateRtt(uint32_t sentTimestampMs, int tickRate) {
        uint32_t now = GetTimestampMs();
        uint32_t rtt = now - sentTimestampMs;

        // ===== FIX #1: Validate RTT before using it =====
        const uint32_t MAX_REASONABLE_RTT = 10000;  // 10 seconds
        const uint32_t MIN_REASONABLE_RTT = 1;      // 1ms minimum

        if (rtt > MAX_REASONABLE_RTT) {
            Debug::Info("DelayCalculator") << "[WARNING] RTT too high: " << rtt
                << "ms (likely clock wrap or packet loss). Ignoring.\n";
            return;
        }

        if (rtt < MIN_REASONABLE_RTT) {
            Debug::Info("DelayCalculator") << "[WARNING] RTT suspiciously low: " << rtt
                << "ms. Ignoring.\n";
            return;
        }

        // ===== FIX #2: Use moving average instead of single sample =====
        m_rttSamples.push_back(rtt);
        if (m_rttSamples.size() > RTT_SAMPLE_WINDOW) {
            m_rttSamples.pop_front();
        }

        // Calculate average of all samples in window
        uint32_t sum = 0;
        for (uint32_t sample : m_rttSamples) {
            sum += sample;
        }
        m_lastRttMs = sum / static_cast<uint32_t>(m_rttSamples.size());
        m_lastLatencyMs = m_lastRttMs / 2.0f;

        CalculateInputDelayFrames(TICKS_PER_SECOND);
    }

    // Accessors
    uint32_t GetLastRttMs() const { return m_lastRttMs; }
    float GetLastLatencyMs() const { return m_lastLatencyMs; }
    int GetInputDelayFrames() const { return m_lastInputDelayFrames; }
    uint32_t m_lastRttMs;
    float m_lastLatencyMs;
    int m_lastInputDelayFrames;

    // ===== FIX #2 (continued): Moving average window =====
    static constexpr size_t RTT_SAMPLE_WINDOW = 5;  // Keep last 5 samples
    std::deque<uint32_t> m_rttSamples;

    void CalculateInputDelayFrames(int tickRate) {
        if (tickRate <= 0) return;

        float frameTimeMs = 1000.0f / tickRate;
        m_lastInputDelayFrames = static_cast<int>(std::ceil(m_lastLatencyMs / frameTimeMs));
    }
};
#pragma once
