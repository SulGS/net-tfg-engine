#include "ConsoleOutput.hpp"
#include <iostream>

static const char* LevelToString(LogLevel level) {
    switch (level) {
    case LogLevel::Info:    return "[INFO]";
    case LogLevel::Warning: return "[WARN]";
    case LogLevel::Error:   return "[ERROR]";
    case LogLevel::Critical:return "[CRITICAL]";
    }
    return "[UNKNOWN]";
}

void ConsoleOutput::Write(const LogMessage& msg) {
    std::cout << LevelToString(msg.level)
        << " [" << msg.channel << "] "
        << msg.text << std::endl;
}
