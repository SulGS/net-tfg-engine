#pragma once
#include <sstream>
#include <string>
#include "LogMessage.hpp"  // only LogLevel

class DebugStream {
public:
    DebugStream(LogLevel level = LogLevel::Info, const std::string& channel = "General");
    ~DebugStream();

    template<typename T>
    DebugStream& operator<<(const T& value) {
        buffer << value;
        return *this;
    }

private:
    std::stringstream buffer;
    LogLevel level;
    std::string channel;
};
