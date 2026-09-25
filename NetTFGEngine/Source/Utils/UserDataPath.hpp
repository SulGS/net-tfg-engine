#pragma once

#include <cstdlib>
#include <filesystem>
#include <string>
#include <system_error>

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
