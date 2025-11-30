#include "DebugStream.hpp"
#include "Debug.hpp"  // include here only

DebugStream::DebugStream(LogLevel level_, const std::string& channel_)
    : level(level_), channel(channel_) {
}

DebugStream::~DebugStream() {
    // Now we can call Debug::Log safely
    switch (level) {
    case LogLevel::Info:
        Debug::Log(buffer.str(), channel);
        break;
    case LogLevel::Warning:
        Debug::LogWarning(buffer.str(), channel);
        break;
    case LogLevel::Error:
        Debug::LogError(buffer.str(), channel);
        break;
    case LogLevel::Critical:
        Debug::LogCritical(buffer.str(), channel);
        break;
    }
}
