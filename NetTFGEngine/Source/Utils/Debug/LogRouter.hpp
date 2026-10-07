#pragma once
#include "LogMessage.hpp"
#include "ThreadSafeQueue.hpp"
#include "FileOutput.hpp"
#include <shared_mutex>
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

    // From a crash handler: lets the worker write out everything still queued and flush the file, waiting at most
    // timeoutMs. Logging stops afterwards (the process is going down).
    void FlushForCrash(unsigned timeoutMs);

private:
    LogRouter();
    ~LogRouter();

    void RouterThread();

    ThreadSafeQueue<LogMessage> queue;

	bool consoleOutputEnabled = false;

    std::atomic<bool> running = false;
    std::atomic<bool> drained = false; // set by the worker once it has emptied the queue and flushed after a Stop
    std::thread worker;

    std::unordered_map<std::string, bool> channelStates;
    mutable std::shared_mutex channelStatesMutex;

    FileOutput fileOutput;
    std::string logFilePath;
};
