#pragma once
#include "LogMessage.hpp"
#include "DebugStream.hpp"
#include <string>

class Debug {
public:
    static void Initialize(const std::string& productName, bool consoleOutput);
	static void Shutdown();

    // Names the calling thread in the log (each line carries the thread it was logged from). `name` must outlive the
    // thread: pass a string literal. Initialize() names its own thread "Main".
    static void SetThreadName(const char* name);

    static void Log(const std::string& msg, const std::string& channel = "General");
    static void LogWarning(const std::string& msg, const std::string& channel = "General");
    static void LogError(const std::string& msg, const std::string& channel = "General");
    static void LogCritical(const std::string& msg, const std::string& channel = "General");

    // The calling code's stack as "module+offset < module+offset ...", innermost first (resolve the offsets against
    // the build's .pdb). For rare diagnostics, e.g. which draw a GL error came from. "" where not supported.
    static std::string CallerStack(int skip = 0, int maxFrames = 12);

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
