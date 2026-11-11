#include "FileOutput.hpp"
#include <chrono>
#include <ctime>
#include <cstdio>

bool FileOutput::Open(const std::string& filepath) {
    file.open(filepath, std::ios::out | std::ios::app);
    return file.is_open();
}

void FileOutput::Write(const LogMessage& msg) {
    if (!file.is_open())
        return;

    auto t = std::chrono::system_clock::to_time_t(msg.timestamp);
    std::tm tm;
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif

    char timeBuf[32];
    std::strftime(timeBuf, sizeof(timeBuf), "%Y-%m-%d %H:%M:%S", &tm);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(msg.timestamp.time_since_epoch()).count() % 1000;

    // "2026-11-10 02:02:16.123 {Render:1234}: [INFO] [Channel] text" (thread: Debug::SetThreadName + OS thread id).
    char prefix[96];
    if (msg.threadName)
        std::snprintf(prefix, sizeof(prefix), "%s.%03d {%s:%u}: ", timeBuf, static_cast<int>(ms), msg.threadName,
            static_cast<unsigned>(msg.osThreadId));
    else
        std::snprintf(prefix, sizeof(prefix), "%s.%03d {%u}: ", timeBuf, static_cast<int>(ms),
            static_cast<unsigned>(msg.osThreadId));

    file << prefix << LevelToString(msg.level) << " ["
        << msg.channel << "] "
        << msg.text;

    // Warnings and errors reach the disk right away: if the process dies next, they are what explains it.
    if (msg.level != LogLevel::Info)
        file.flush();
}

void FileOutput::Flush() {
    if (file.is_open())
        file.flush();
}

void FileOutput::Close() {
    if (file.is_open())
        file.close();
}
