#pragma once
// System description written to the log once at startup (channel "System"), so a log sent by a tester says what it
// ran on. Included only from Debug.cpp (Debug::Initialize calls SystemInfo::Log): it pulls in OS headers.
//
// Per OS:  Windows - registry (OS name/version, CPU name), Win32 (cores, memory, locale).
//          Linux   - /etc/os-release, uname, /proc/cpuinfo (+ device-tree model on ARM boards), /proc/meminfo.
//          other   - uname + sysconf only.
// The GPU and the display are logged by the client when the window/GL context exist (OpenGLWindow), not here: the
// headless server has neither.

#include "Debug.hpp"
#include <cstdint>
#include <cstdlib>
#include <sstream>
#include <iomanip>
#include <string>
#include <thread>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <vector>
#pragma comment(lib, "advapi32.lib")
#else
#include <sys/utsname.h>
#include <unistd.h>
#include <fstream>
#include <set>
#endif

namespace SystemInfo {

    inline std::string Trim(std::string s) {
        const char* ws = " \t\r\n";
        s.erase(0, s.find_first_not_of(ws));
        const size_t end = s.find_last_not_of(ws);
        s.erase(end == std::string::npos ? 0 : end + 1);
        return s;
    }

    inline std::string Gb(uint64_t bytes) {
        std::ostringstream o;
        o << std::fixed << std::setprecision(1) << bytes / (1024.0 * 1024.0 * 1024.0) << " GB";
        return o.str();
    }

    inline const char* BuildArch() {
#if defined(_M_X64) || defined(__x86_64__)
        return "x64";
#elif defined(_M_ARM64) || defined(__aarch64__)
        return "ARM64";
#elif defined(_M_IX86) || defined(__i386__)
        return "x86";
#elif defined(__arm__)
        return "ARM";
#else
        return "unknown arch";
#endif
    }

    inline std::string BuildDescription() {
        std::ostringstream o;
#if defined(DIST)
        o << "Dist";
#elif defined(RELEASE)
        o << "Release";
#elif defined(DEBUG) || defined(_DEBUG)
        o << "Debug";
#else
        o << "?";
#endif
        o << " | " << BuildArch() << " | ";
#if defined(_MSC_VER) && !defined(__clang__)
        o << "MSVC " << _MSC_VER / 100 << "." << _MSC_VER % 100;
#elif defined(__clang__)
        o << "Clang " << __clang_major__ << "." << __clang_minor__;
#elif defined(__GNUC__)
        o << "GCC " << __GNUC__ << "." << __GNUC_MINOR__;
#endif
        o << " | built " << __DATE__ << " " << __TIME__;
        return o.str();
    }

#ifdef _WIN32
    inline std::string RegString(const char* path, const char* name) {
        char buf[512] = {};
        DWORD size = sizeof(buf);
        if (RegGetValueA(HKEY_LOCAL_MACHINE, path, name, RRF_RT_REG_SZ, nullptr, buf, &size) != ERROR_SUCCESS)
            return {};
        return Trim(buf);
    }

    inline DWORD RegDword(const char* path, const char* name) {
        DWORD value = 0, size = sizeof(value);
        if (RegGetValueA(HKEY_LOCAL_MACHINE, path, name, RRF_RT_REG_DWORD, nullptr, &value, &size) != ERROR_SUCCESS)
            return 0;
        return value;
    }

    inline std::string OsDescription() {
        const char* key = "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion";
        std::string product = RegString(key, "ProductName");
        const std::string display = RegString(key, "DisplayVersion");   // "23H2" (Windows 10 20H2+)
        const std::string build = RegString(key, "CurrentBuildNumber");
        const DWORD ubr = RegDword(key, "UBR");
        // Windows 11 still reports "Windows 10" in ProductName: tell them apart by build number (11 = 22000+).
        if (!build.empty() && std::atoi(build.c_str()) >= 22000) {
            const size_t p = product.find("Windows 10");
            if (p != std::string::npos) product.replace(p, 10, "Windows 11");
        }
        std::ostringstream o;
        o << (product.empty() ? "Windows" : product);
        if (!display.empty()) o << " " << display;
        if (!build.empty()) o << " (build " << build << (ubr ? "." + std::to_string(ubr) : "") << ")";

        BOOL wow64 = FALSE;
        if (IsWow64Process(GetCurrentProcess(), &wow64) && wow64) o << " | 32-bit process on 64-bit Windows";

        wchar_t locale[LOCALE_NAME_MAX_LENGTH] = {};
        if (GetUserDefaultLocaleName(locale, LOCALE_NAME_MAX_LENGTH) > 0) {
            std::string narrow;
            for (const wchar_t* c = locale; *c; ++c) narrow += static_cast<char>(*c < 128 ? *c : '?');
            o << " | locale " << narrow;
        }
        return o.str();
    }

    inline std::string CpuDescription() {
        std::string name = RegString("HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", "ProcessorNameString");
        const DWORD mhz = RegDword("HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", "~MHz");

        unsigned cores = 0;
        DWORD len = 0;
        GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &len);
        if (len > 0) {
            std::vector<char> buf(len);
            auto* info = reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buf.data());
            if (GetLogicalProcessorInformationEx(RelationProcessorCore, info, &len)) {
                for (DWORD off = 0; off < len;) {
                    auto* p = reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buf.data() + off);
                    if (p->Relationship == RelationProcessorCore) ++cores;
                    off += p->Size;
                }
            }
        }
        const unsigned threads = static_cast<unsigned>(GetActiveProcessorCount(ALL_PROCESSOR_GROUPS));

        std::ostringstream o;
        o << (name.empty() ? "unknown CPU" : name);
        if (cores) o << " | " << cores << " cores";
        o << " / " << threads << " threads";
        if (mhz) o << " | " << mhz << " MHz";
        return o.str();
    }

    inline std::string MemoryDescription() {
        MEMORYSTATUSEX ms{};
        ms.dwLength = sizeof(ms);
        if (!GlobalMemoryStatusEx(&ms))
            return "unknown";
        return Gb(ms.ullTotalPhys) + " total, " + Gb(ms.ullAvailPhys) + " available (" +
            std::to_string(ms.dwMemoryLoad) + "% in use)";
    }

#else // POSIX

    inline std::string ReadKeyValue(const char* path, const std::string& key, char sep) {
        std::ifstream f(path);
        std::string line;
        while (std::getline(f, line)) {
            const size_t p = line.find(sep);
            if (p != std::string::npos && Trim(line.substr(0, p)) == key)
                return Trim(line.substr(p + 1));
        }
        return {};
    }

    inline std::string OsDescription() {
        std::ostringstream o;
#ifdef __linux__
        std::string pretty = ReadKeyValue("/etc/os-release", "PRETTY_NAME", '=');
        if (pretty.size() >= 2 && pretty.front() == '"' && pretty.back() == '"')
            pretty = pretty.substr(1, pretty.size() - 2);
        o << (pretty.empty() ? "Linux" : pretty);
#endif
        utsname u{};
        if (uname(&u) == 0) {
#ifdef __linux__
            o << " | kernel " << u.release << " " << u.machine;
#else
            o << u.sysname << " " << u.release << " " << u.machine;
#endif
        }
        return o.str();
    }

    inline std::string CpuDescription() {
        std::string name;
        unsigned cores = 0;
#ifdef __linux__
        name = ReadKeyValue("/proc/cpuinfo", "model name", ':');          // x86
        if (name.empty()) name = ReadKeyValue("/proc/cpuinfo", "Model", ':');      // Raspberry Pi
        if (name.empty()) name = ReadKeyValue("/proc/cpuinfo", "Hardware", ':');   // other ARM kernels
        if (name.empty()) {                                                // ARM boards: device-tree model
            std::ifstream dt("/sys/firmware/devicetree/base/model");
            std::getline(dt, name, '\0');
            name = Trim(name);
        }
        // Physical cores: distinct (physical id, core id) pairs; absent on most ARM kernels.
        {
            std::ifstream f("/proc/cpuinfo");
            std::set<std::pair<std::string, std::string>> seen;
            std::string line, phys = "0";
            while (std::getline(f, line)) {
                const size_t p = line.find(':');
                if (p == std::string::npos) continue;
                const std::string k = Trim(line.substr(0, p)), v = Trim(line.substr(p + 1));
                if (k == "physical id") phys = v;
                else if (k == "core id") seen.insert({ phys, v });
            }
            cores = static_cast<unsigned>(seen.size());
        }
#endif
        const long threads = sysconf(_SC_NPROCESSORS_ONLN);
        std::ostringstream o;
        o << (name.empty() ? "unknown CPU" : name);
        if (cores) o << " | " << cores << " cores";
        o << " / " << (threads > 0 ? threads : static_cast<long>(std::thread::hardware_concurrency())) << " threads";
        return o.str();
    }

    inline std::string MemoryDescription() {
        const long pages = sysconf(_SC_PHYS_PAGES), pageSize = sysconf(_SC_PAGE_SIZE);
        std::string out = (pages > 0 && pageSize > 0)
            ? Gb(static_cast<uint64_t>(pages) * static_cast<uint64_t>(pageSize)) + " total" : "unknown";
#ifdef __linux__
        const std::string avail = ReadKeyValue("/proc/meminfo", "MemAvailable", ':'); // "7864320 kB"
        if (!avail.empty())
            out += ", " + Gb(std::strtoull(avail.c_str(), nullptr, 10) * 1024ULL) + " available";
#endif
        return out;
    }
#endif

    inline void Log() {
        // Each piece is guarded on its own: a missing registry key or /proc file just leaves that field "unknown".
        Debug::Info("System") << "OS: " << OsDescription() << "\n";
        Debug::Info("System") << "CPU: " << CpuDescription() << "\n";
        Debug::Info("System") << "RAM: " << MemoryDescription() << "\n";
        Debug::Info("System") << "Build: " << BuildDescription() << "\n";
    }
}
