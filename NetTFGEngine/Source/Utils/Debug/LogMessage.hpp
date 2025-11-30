#pragma once
#include <string>
#include <chrono>
#include <thread>

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
};
