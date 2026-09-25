#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

// A launched game server. Tied to the matchmaker's lifetime (job object / PR_SET_PDEATHSIG) so it never holds a port
// after it. Output never reaches the matchmaker: own console window (`title`) on Windows, discarded on Linux (log files).
class ChildProcess {
public:
    // Returns nullptr and fills `error` if the process could not be started.
    static std::unique_ptr<ChildProcess> Launch(const std::filesystem::path& executable,
        const std::vector<std::string>& args,
        const std::filesystem::path& workingDirectory,
        const std::string& title,
        std::string& error);

    ~ChildProcess();

    ChildProcess(const ChildProcess&) = delete;
    ChildProcess& operator=(const ChildProcess&) = delete;

    // Non-blocking. Once it returns false, ExitCode() is valid.
    bool IsRunning();
    int ExitCode() const { return exitCode_; }

    // Terminates the process and waits for it.
    void Kill();

    long Pid() const { return pid_; }

private:
    ChildProcess() = default;

    long pid_ = 0;
    bool exited_ = false;
    int exitCode_ = 0;
#ifdef _WIN32
    void* handle_ = nullptr;
#endif
};

// Folder of the running executable (used to find GameServer next to the matchmaker).
std::filesystem::path CurrentExecutableDirectory();
