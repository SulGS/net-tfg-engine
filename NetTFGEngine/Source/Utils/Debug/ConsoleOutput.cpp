#include "ConsoleOutput.hpp"
#include <iostream>

void ConsoleOutput::Write(const LogMessage& msg) {
    std::cout << LevelToString(msg.level)
        << " [" << msg.channel << "] "
        << msg.text;
}
