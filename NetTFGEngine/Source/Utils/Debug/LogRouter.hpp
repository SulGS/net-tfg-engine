#pragma once
#include "LogMessage.hpp"
#include "ThreadSafeQueue.hpp"
#include "FileOutput.hpp"
#include <unordered_map>
#include <string>
#include <thread>
#include <atomic>

class LogRouter {
public:
    static LogRouter& Instance();

    void Start(bool consoleOutput);
    void Stop();

    void SetProductName(const std::string& name);
    void SetChannelEnabled(const std::string& channel, bool enabled);

    void Enqueue(const LogMessage& msg);

private:
    LogRouter();
    ~LogRouter();

    void RouterThread();

    ThreadSafeQueue<LogMessage> queue;

	bool consoleOutputEnabled = false;

    std::atomic<bool> running = false;
    std::thread worker;

    std::unordered_map<std::string, bool> channelStates;

    FileOutput fileOutput;
    std::string logFilePath;
};
