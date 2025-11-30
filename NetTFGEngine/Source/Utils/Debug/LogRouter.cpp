#include "LogRouter.hpp"
#include "ConsoleOutput.hpp"
#include <filesystem>
#include <iostream>
#include <chrono>
#include <sstream>
#include <iomanip>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

LogRouter& LogRouter::Instance() {
    static LogRouter instance;
    return instance;
}

LogRouter::LogRouter() {}
LogRouter::~LogRouter() {
    Stop();
}

void LogRouter::Start() {
    if (running)
        return;

    running = true;
    worker = std::thread(&LogRouter::RouterThread, this);
}

void LogRouter::Stop() {
    if (!running)
        return;

    running = false;
    queue.Stop();

    if (worker.joinable())
        worker.join();

    fileOutput.Close();
}

void LogRouter::SetProductName(const std::string& name) {
#ifdef _WIN32
    char* localAppData = nullptr;
    size_t len = 0;
    _dupenv_s(&localAppData, &len, "LOCALAPPDATA");
    if (!localAppData)
        return;
    std::string folder = std::string(localAppData) + "\\NetTFGEngine\\" + name + "\\logs\\";
    free(localAppData);
#else
    std::string folder = std::string(getenv("HOME")) + "/.NetTFGEngine/" + name + "/logs/";
#endif

    std::filesystem::create_directories(folder);

    // --- Timestamp ---
    auto now = std::chrono::system_clock::now();
    auto time = std::chrono::system_clock::to_time_t(now);
    std::tm tm;
#ifdef _WIN32
    localtime_s(&tm, &time);
#else
    localtime_r(&time, &tm);
#endif
    std::stringstream timestamp;
    timestamp << std::put_time(&tm, "%Y%m%d_%H%M%S");

    // --- Process ID ---
#ifdef _WIN32
    DWORD pid = GetCurrentProcessId();
#else
    pid_t pid = getpid();
#endif

    // --- Build final log filename ---
    logFilePath = folder + "engine_" + timestamp.str() + "_PID" + std::to_string(pid) + ".log";

    fileOutput.Open(logFilePath);
}


void LogRouter::SetChannelEnabled(const std::string& channel, bool enabled) {
    channelStates[channel] = enabled;
}

void LogRouter::Enqueue(const LogMessage& msg) {
    queue.Push(msg);
}

void LogRouter::RouterThread() {
    while (running) {
        LogMessage msg;

        if (!queue.WaitPop(msg))
            break; // stopping

        // Channel filtering
        auto it = channelStates.find(msg.channel);
        if (it != channelStates.end() && !it->second)
            continue;

        ConsoleOutput::Write(msg);

        fileOutput.Write(msg);
    }
}
