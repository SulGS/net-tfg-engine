#include "ChildProcess.hpp"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <sys/prctl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#ifdef _WIN32

namespace
{
    // All children go into this job; closing its last handle (the matchmaker exiting,
    // even by a crash) kills them.
    HANDLE ChildrenJob() {
        static HANDLE job = []() {
            HANDLE h = CreateJobObjectA(nullptr, nullptr);
            if (h) {
                JOBOBJECT_EXTENDED_LIMIT_INFORMATION info = {};
                info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
                SetInformationJobObject(h, JobObjectExtendedLimitInformation, &info, sizeof(info));
            }
            return h;
        }();
        return job;
    }

    // Quotes one argument following the CommandLineToArgvW rules.
    std::string QuoteArgument(const std::string& arg) {
        if (!arg.empty() && arg.find_first_of(" \t\"") == std::string::npos) {
            return arg;
        }

        std::string quoted = "\"";
        size_t backslashes = 0;
        for (char c : arg) {
            if (c == '\\') {
                ++backslashes;
                continue;
            }
            if (c == '"') {
                quoted.append(backslashes * 2 + 1, '\\');
            }
            else {
                quoted.append(backslashes, '\\');
            }
            backslashes = 0;
            quoted.push_back(c);
        }
        quoted.append(backslashes * 2, '\\');
        quoted.push_back('"');
        return quoted;
    }
}

std::unique_ptr<ChildProcess> ChildProcess::Launch(const std::filesystem::path& executable,
    const std::vector<std::string>& args,
    const std::filesystem::path& workingDirectory,
    const std::string& title,
    std::string& error)
{
    std::string commandLine = QuoteArgument(executable.string());
    for (const auto& arg : args) {
        commandLine += " " + QuoteArgument(arg);
    }

    std::string windowTitle = title;
    STARTUPINFOA startup = {};
    startup.cb = sizeof(startup);
    startup.lpTitle = windowTitle.empty() ? nullptr : windowTitle.data();
    PROCESS_INFORMATION info = {};

    // Own console so its logs don't mix with the matchmaker's.
    // Suspended so it joins the job before it can run any code.
    if (!CreateProcessA(executable.string().c_str(), commandLine.data(), nullptr, nullptr, FALSE,
        CREATE_NEW_CONSOLE | CREATE_SUSPENDED, nullptr, workingDirectory.string().c_str(), &startup, &info)) {
        error = "CreateProcess failed with error " + std::to_string(GetLastError());
        return nullptr;
    }

    if (HANDLE job = ChildrenJob()) {
        AssignProcessToJobObject(job, info.hProcess);
    }
    ResumeThread(info.hThread);
    CloseHandle(info.hThread);

    std::unique_ptr<ChildProcess> child(new ChildProcess());
    child->handle_ = info.hProcess;
    child->pid_ = static_cast<long>(info.dwProcessId);
    return child;
}

ChildProcess::~ChildProcess() {
    if (handle_) {
        CloseHandle(static_cast<HANDLE>(handle_));
    }
}

bool ChildProcess::IsRunning() {
    if (exited_) {
        return false;
    }

    DWORD code = 0;
    if (!GetExitCodeProcess(static_cast<HANDLE>(handle_), &code) || code != STILL_ACTIVE) {
        exited_ = true;
        exitCode_ = static_cast<int>(code);
        return false;
    }
    return true;
}

void ChildProcess::Kill() {
    if (!IsRunning()) {
        return;
    }
    TerminateProcess(static_cast<HANDLE>(handle_), 1);
    WaitForSingleObject(static_cast<HANDLE>(handle_), 5000);
    IsRunning();
}

std::filesystem::path CurrentExecutableDirectory() {
    char buffer[MAX_PATH];
    DWORD len = GetModuleFileNameA(nullptr, buffer, MAX_PATH);
    if (len == 0 || len == MAX_PATH) {
        return std::filesystem::current_path();
    }
    return std::filesystem::path(std::string(buffer, len)).parent_path();
}

#else

std::unique_ptr<ChildProcess> ChildProcess::Launch(const std::filesystem::path& executable,
    const std::vector<std::string>& args,
    const std::filesystem::path& workingDirectory,
    const std::string& /*title*/,
    std::string& error)
{
    // Build argv before forking: only async-signal-safe calls are allowed in the child.
    std::string exe = executable.string();
    std::string cwd = workingDirectory.string();
    std::vector<char*> argv;
    argv.push_back(exe.data());
    std::vector<std::string> argsCopy = args;
    for (auto& arg : argsCopy) {
        argv.push_back(arg.data());
    }
    argv.push_back(nullptr);

    const pid_t parent = getpid();
    pid_t pid = fork();
    if (pid < 0) {
        error = std::string("fork failed: ") + std::strerror(errno);
        return nullptr;
    }

    if (pid == 0) {
        prctl(PR_SET_PDEATHSIG, SIGTERM);
        // The parent may have died between fork() and prctl().
        if (getppid() != parent) {
            _exit(1);
        }
        if (chdir(cwd.c_str()) != 0) {
            _exit(126);
        }
        // A headless server has no terminal to give each child, so keep their output
        // out of the matchmaker's; every GameServer still writes its own log file.
        int devNull = open("/dev/null", O_WRONLY);
        if (devNull >= 0) {
            dup2(devNull, STDOUT_FILENO);
            dup2(devNull, STDERR_FILENO);
            close(devNull);
        }
        execv(exe.c_str(), argv.data());
        _exit(127);
    }

    std::unique_ptr<ChildProcess> child(new ChildProcess());
    child->pid_ = static_cast<long>(pid);
    return child;
}

ChildProcess::~ChildProcess() {
    // Reap it if already finished so it doesn't linger as a zombie.
    if (!exited_ && pid_ > 0) {
        IsRunning();
    }
}

bool ChildProcess::IsRunning() {
    if (exited_) {
        return false;
    }

    int status = 0;
    pid_t result = waitpid(static_cast<pid_t>(pid_), &status, WNOHANG);
    if (result == 0) {
        return true;
    }

    exited_ = true;
    if (result > 0) {
        exitCode_ = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    }
    return false;
}

void ChildProcess::Kill() {
    if (!IsRunning()) {
        return;
    }
    kill(static_cast<pid_t>(pid_), SIGKILL);
    int status = 0;
    waitpid(static_cast<pid_t>(pid_), &status, 0);
    exited_ = true;
    exitCode_ = -1;
}

std::filesystem::path CurrentExecutableDirectory() {
    std::error_code ec;
    auto exe = std::filesystem::read_symlink("/proc/self/exe", ec);
    if (ec) {
        return std::filesystem::current_path();
    }
    return exe.parent_path();
}

#endif
