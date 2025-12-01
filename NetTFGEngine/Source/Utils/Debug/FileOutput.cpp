#include "FileOutput.hpp"
#include <chrono>
#include <ctime>

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

    file << timeBuf << ": " << LevelToString(msg.level) << " ["
        << msg.channel << "] "
        << msg.text;
}

void FileOutput::Close() {
    if (file.is_open())
        file.close();
}
