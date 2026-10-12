#pragma once

#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <string>
#include <system_error>

#ifdef _WIN32
#include <process.h>   // _getpid: avoids pulling <windows.h> into every includer
#else
#include <unistd.h>
#endif

// Per-user writable storage (settings, logs): Windows %LOCALAPPDATA%\NetTFGEngine\<product>\, Linux
// $HOME/.NetTFGEngine/<product>/, so the game works installed in a read-only location (e.g. Program Files).
namespace UserDataPath
{
    inline std::string& ProductNameStorage()
    {
        static std::string name;
        return name;
    }

    // Called once from Debug::Initialize, before any settings file is touched.
    inline void SetProductName(const std::string& name)
    {
        ProductNameStorage() = name;
    }

    // <base>/NetTFGEngine/<product>/. Empty when the base directory can't be resolved.
    inline std::filesystem::path Directory()
    {
        std::filesystem::path base;
#ifdef _WIN32
        char* localAppData = nullptr;
        size_t len = 0;
        _dupenv_s(&localAppData, &len, "LOCALAPPDATA");
        if (!localAppData)
            return {};
        base = std::filesystem::path(localAppData) / "NetTFGEngine";
        free(localAppData);
#else
        const char* home = std::getenv("HOME");
        if (!home)
            return {};
        base = std::filesystem::path(home) / ".NetTFGEngine";
#endif
        return ProductNameStorage().empty() ? base : base / ProductNameStorage();
    }

    inline std::filesystem::path LogsDirectory()
    {
        auto dir = Directory();
        return dir.empty() ? dir : dir / "logs";
    }

    // This run's own folder inside LogsDirectory(): logs/<YYYYMMDD_HHMMSS>_PID<pid>/, so every launch (and every
    // client/server instance started together) keeps its log and crash reports apart. Fixed on the first call, which
    // must come after SetProductName (Debug::Initialize does both). Not created here; empty if LogsDirectory() is.
    inline std::filesystem::path SessionLogsDirectory()
    {
        static const std::filesystem::path dir = []() -> std::filesystem::path
        {
            const auto logs = LogsDirectory();
            if (logs.empty())
                return {};

            const std::time_t now = std::time(nullptr);
            std::tm tm{};
#ifdef _WIN32
            localtime_s(&tm, &now);
            const unsigned long pid = static_cast<unsigned long>(_getpid());
#else
            localtime_r(&now, &tm);
            const unsigned long pid = static_cast<unsigned long>(getpid());
#endif
            char stamp[32];
            std::strftime(stamp, sizeof(stamp), "%Y%m%d_%H%M%S", &tm);
            return logs / (std::string(stamp) + "_PID" + std::to_string(pid));
        }();
        return dir;
    }

    // Full path for a file inside Directory(), creating the folder if needed.
    // Falls back to the bare filename (working directory) if the folder is unavailable.
    inline std::string File(const std::string& filename)
    {
        auto dir = Directory();
        if (dir.empty())
            return filename;

        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        if (ec)
            return filename;

        return (dir / filename).string();
    }
}
