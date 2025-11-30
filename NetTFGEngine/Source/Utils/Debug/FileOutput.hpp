#pragma once
#include "LogMessage.hpp"
#include <fstream>
#include <string>

class FileOutput {
public:
    FileOutput() = default;
    bool Open(const std::string& filepath);
    void Write(const LogMessage& msg);
    void Close();

private:
    std::ofstream file;
};
