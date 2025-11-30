#pragma once
#include "LogMessage.hpp"
#include "DebugStream.hpp"
#include <string>

class Debug {
public:
    static void Initialize(const std::string& productName);

    static void Log(const std::string& msg, const std::string& channel = "General");
    static void LogWarning(const std::string& msg, const std::string& channel = "General");
    static void LogError(const std::string& msg, const std::string& channel = "General");
    static void LogCritical(const std::string& msg, const std::string& channel = "General");

    static DebugStream Info(const std::string& channel = "General") {
        return DebugStream(LogLevel::Info, channel);
    }
    static DebugStream Warning(const std::string& channel = "General") {
        return DebugStream(LogLevel::Warning, channel);
    }
    static DebugStream Error(const std::string& channel = "General") {
        return DebugStream(LogLevel::Error, channel);
    }

	static DebugStream Critical(const std::string& channel = "General") {
		return DebugStream(LogLevel::Critical, channel);
	}
};
