#include "LogRouter.hpp"
#include "ConsoleOutput.hpp"
#include "Utils/UserDataPath.hpp"
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

void LogRouter::Start(bool consoleOutput) {
    if (running)
        return;

	consoleOutputEnabled = consoleOutput;

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
    UserDataPath::SetProductName(name);
    const std::filesystem::path logsDir = UserDataPath::LogsDirectory();
    if (logsDir.empty())
        return;

    std::filesystem::create_directories(logsDir);
    const std::string folder = (logsDir / "").string();

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

#ifdef _WIN32
    DWORD pid = GetCurrentProcessId();
#else
    pid_t pid = getpid();
#endif

    logFilePath = folder + "engine_" + timestamp.str() + "_PID" + std::to_string(pid) + ".log";

    fileOutput.Open(logFilePath);
}


void LogRouter::SetChannelEnabled(const std::string& channel, bool enabled) {
    std::unique_lock lock(channelStatesMutex);
    channelStates[channel] = enabled;
}

void LogRouter::Enqueue(const LogMessage& msg) {
    queue.Push(msg);
}

void LogRouter::RouterThread() {
    try {
        while (running) {
            LogMessage msg;

            if (!queue.WaitPop(msg))
                break; // stopping

            {
                std::shared_lock lock(channelStatesMutex);
                auto it = channelStates.find(msg.channel);
                if (it != channelStates.end() && !it->second)
                    continue;
            }

            try {
                if (consoleOutputEnabled) ConsoleOutput::Write(msg);
            }
            catch (const std::exception& e) {
                // Log to file only to avoid recursive errors
                LogMessage errorMsg;
                errorMsg.level = LogLevel::Error;
                errorMsg.text = "ConsoleOutput::Write failed: " + std::string(e.what()) + "\n";
                errorMsg.channel = "LogRouter";
                errorMsg.timestamp = std::chrono::system_clock::now();
                errorMsg.threadId = std::this_thread::get_id();
                fileOutput.Write(errorMsg);
            }
            catch (...) {
                LogMessage errorMsg;
                errorMsg.level = LogLevel::Error;
                errorMsg.text = "ConsoleOutput::Write failed with unknown exception\n";
                errorMsg.channel = "LogRouter";
                errorMsg.timestamp = std::chrono::system_clock::now();
                errorMsg.threadId = std::this_thread::get_id();
                fileOutput.Write(errorMsg);
            }

            try {
                fileOutput.Write(msg);
            }
            catch (const std::exception& e) {
                // Can't log this anywhere safely, but catch to prevent thread crash
#ifdef _WIN32
                OutputDebugStringA(("FileOutput::Write failed: " + std::string(e.what()) + "\n").c_str());
#endif
            }
            catch (...) {
#ifdef _WIN32
                OutputDebugStringA("FileOutput::Write failed with unknown exception\n");
#endif
            }
        }
    }
    catch (const std::exception& e) {
#ifdef _WIN32
        OutputDebugStringA(("LogRouter thread crashed: " + std::string(e.what()) + "\n").c_str());
#endif
        running = false;
    }
    catch (...) {
#ifdef _WIN32
        OutputDebugStringA("LogRouter thread crashed with unknown exception\n");
#endif
        running = false;
    }
}
