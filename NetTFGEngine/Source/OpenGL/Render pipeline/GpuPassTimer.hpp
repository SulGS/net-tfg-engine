#pragma once

// GPU time per render pass, logged once per second as "[GPUPasses] total X ms | Shading a (max b) | ...".
// Mark(label) drops a GL_TIMESTAMP query; the time up to the next Mark (or EndFrame) is charged to that label. Results
// are read kRing frames later, when the GPU is long done with them, so nothing here stalls the pipeline. Render
// thread only (needs the GL context).

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <sstream>
#include <iomanip>
#include <vector>

#include "OpenGL/OpenGLIncludes.hpp"
#include "Utils/Debug/Debug.hpp"

class GpuPassTimer {
public:
    // Labels must be string literals (stored by pointer, compared by content).
    void BeginFrame()
    {
        if (!m_enabled) return;
        if (!m_init) {
            for (Slot& s : m_ring) glGenQueries(kMaxMarks, s.queries);
            m_init = true;
            m_windowStart = std::chrono::steady_clock::now();
        }
        m_cur = (m_cur + 1) % kRing;
        Collect(m_ring[m_cur]);   // the frame that used this slot kRing frames ago
        m_ring[m_cur].count = 0;
        m_inFrame = true;
    }

    void Mark(const char* label)
    {
        if (!m_inFrame) return;
        Slot& s = m_ring[m_cur];
        if (s.count >= kMaxMarks - 1) return;   // keep one query for EndFrame
        glQueryCounter(s.queries[s.count], GL_TIMESTAMP);
        s.labels[s.count] = label;
        s.count++;
    }

    // `extra` is appended to the per-second line (e.g. how many shadow lights were rendered).
    void EndFrame(int shadowLights, int shadowCasterDraws)
    {
        if (!m_inFrame) return;
        Slot& s = m_ring[m_cur];
        glQueryCounter(s.queries[s.count], GL_TIMESTAMP);
        s.labels[s.count] = nullptr;
        s.count++;
        s.shadowLights = shadowLights;
        s.casterDraws = shadowCasterDraws;
        m_inFrame = false;

        const auto now = std::chrono::steady_clock::now();
        if (now - m_windowStart >= std::chrono::seconds(1)) {
            Flush();
            m_windowStart = now;
        }
    }

    // A frame that ended early (e.g. no camera): its marks are dropped.
    void AbortFrame()
    {
        if (!m_inFrame) return;
        m_ring[m_cur].count = 0;
        m_inFrame = false;
    }

    void Release()
    {
        if (m_init)
            for (Slot& s : m_ring) glDeleteQueries(kMaxMarks, s.queries);
        m_init = false;
        m_inFrame = false;
        for (Slot& s : m_ring) s.count = 0;
        m_stats.clear();
        m_frames = 0;
    }

    void SetEnabled(bool on) { if (!on) AbortFrame(); m_enabled = on; }

private:
    static constexpr int kRing = 4;
    static constexpr int kMaxMarks = 24;

    struct Slot {
        GLuint queries[kMaxMarks] = {};
        const char* labels[kMaxMarks] = {};
        int count = 0;
        int shadowLights = 0;
        int casterDraws = 0;
    };
    struct Stat { const char* label; double sumMs; double maxMs; };

    void Collect(Slot& s)
    {
        if (s.count < 2) return;
        GLint available = 0;
        glGetQueryObjectiv(s.queries[s.count - 1], GL_QUERY_RESULT_AVAILABLE, &available);
        if (!available) return;   // GPU >kRing frames behind: skip rather than stall

        GLuint64 prev = 0;
        glGetQueryObjectui64v(s.queries[0], GL_QUERY_RESULT, &prev);
        const GLuint64 first = prev;
        for (int i = 1; i < s.count; i++) {
            GLuint64 t = 0;
            glGetQueryObjectui64v(s.queries[i], GL_QUERY_RESULT, &t);
            Add(s.labels[i - 1], double(t - prev) / 1.0e6);
            prev = t;
        }
        Add("total", double(prev - first) / 1.0e6);
        m_shadowLightsSum += s.shadowLights;
        m_shadowLightsMax = std::max(m_shadowLightsMax, s.shadowLights);
        m_casterDrawsSum += s.casterDraws;
        m_frames++;
    }

    void Add(const char* label, double ms)
    {
        for (Stat& st : m_stats)
            if (st.label == label || std::strcmp(st.label, label) == 0) {
                st.sumMs += ms;
                if (ms > st.maxMs) st.maxMs = ms;
                return;
            }
        m_stats.push_back(Stat{ label, ms, ms });
    }

    void Flush()
    {
        if (m_frames == 0) return;
        std::ostringstream line;
        line << std::fixed << std::setprecision(2);
        for (const Stat& st : m_stats)
            if (std::strcmp(st.label, "total") == 0)
                line << "total " << st.sumMs / m_frames << " ms (max " << st.maxMs << ")";
        for (const Stat& st : m_stats) {
            if (std::strcmp(st.label, "total") == 0) continue;
            const double avg = st.sumMs / m_frames;
            if (avg < 0.05 && st.maxMs < 0.5) continue;   // keep the line short: idle passes left out
            line << " | " << st.label << " " << avg << " (max " << st.maxMs << ")";
        }
        line << " | shadow lights avg " << double(m_shadowLightsSum) / m_frames << " max " << m_shadowLightsMax
             << " | caster draws/frame " << double(m_casterDrawsSum) / m_frames
             << " | " << m_frames << " frames";
        Debug::Info("GPUPasses") << line.str() << "\n";

        m_stats.clear();
        m_frames = 0;
        m_shadowLightsSum = 0;
        m_shadowLightsMax = 0;
        m_casterDrawsSum = 0;
    }

    Slot m_ring[kRing];
    int  m_cur = 0;
    bool m_init = false;
    bool m_inFrame = false;
    bool m_enabled = true;

    std::vector<Stat> m_stats;
    int  m_frames = 0;
    long long m_shadowLightsSum = 0;
    int  m_shadowLightsMax = 0;
    long long m_casterDrawsSum = 0;
    std::chrono::steady_clock::time_point m_windowStart{};
};
