#include "Debug.hpp"
#include "LogRouter.hpp"
#include "Utils/UserDataPath.hpp"

static LogMessage CreateMessage(LogLevel level, const std::string& text, const std::string& channel) {
    LogMessage msg;
    msg.level = level;
    msg.text = text;
    msg.channel = channel;
    msg.timestamp = std::chrono::system_clock::now();
    msg.threadId = std::this_thread::get_id();
    return msg;
}

void Debug::Initialize(const std::string& productName, bool consoleOutput) {
    UserDataPath::SetProductName(productName);
    LogRouter::Instance().SetProductName(productName);
    LogRouter::Instance().Start(consoleOutput);
}

void Debug::Shutdown() {
	LogRouter::Instance().Stop();
}

void Debug::Log(const std::string& msg, const std::string& channel) {
    LogRouter::Instance().Enqueue(CreateMessage(LogLevel::Info, msg, channel));
}

void Debug::LogWarning(const std::string& msg, const std::string& channel) {
    LogRouter::Instance().Enqueue(CreateMessage(LogLevel::Warning, msg, channel));
}

void Debug::LogError(const std::string& msg, const std::string& channel) {
    LogRouter::Instance().Enqueue(CreateMessage(LogLevel::Error, msg, channel));
}

void Debug::LogCritical(const std::string& msg, const std::string& channel) {
    LogRouter::Instance().Enqueue(CreateMessage(LogLevel::Critical, msg, channel));
}
