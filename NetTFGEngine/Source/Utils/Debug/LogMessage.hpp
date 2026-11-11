#pragma once
#include <string>
#include <chrono>
#include <thread>
#include <cstdint>

enum class LogLevel {
    Info,
    Warning,
    Error,
    Critical
};

struct LogMessage {
    LogLevel level;
    std::string text;
    std::string channel;
    std::chrono::system_clock::time_point timestamp;
    std::thread::id threadId;
    uint32_t osThreadId = 0;           // OS id (same one the debugger and crash.txt show)
    const char* threadName = nullptr;  // Debug::SetThreadName of the logging thread, or null
};

static const char* LevelToString(LogLevel level) {
    switch (level) {
    case LogLevel::Info:    return "[INFO]";
    case LogLevel::Warning: return "[WARN]";
    case LogLevel::Error:   return "[ERROR]";
    case LogLevel::Critical:return "[CRITICAL]";
    }
    return "[UNKNOWN]";
}