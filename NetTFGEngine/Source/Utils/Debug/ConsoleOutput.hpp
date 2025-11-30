#pragma once
#include "LogMessage.hpp"

class ConsoleOutput {
public:
    static void Write(const LogMessage& msg);
};
