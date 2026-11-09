#pragma once

#include <cstddef>
#include <mutex>
#include <string>

#include "Utils/Debug/Debug.hpp"

// Version of the game built on the engine (not of the engine or of a wire protocol). The game sets it once at startup,
// on both client and server; the engine then logs it, sends it in CLIENT_HELLO and to the matchmaker, and rejects peers
// whose version differs. Unset ("") means "any": nothing is checked against it, so tools/tests need not set one.
namespace GameVersion
{
    // Fits ClientHelloPacket::gameVersion (MAX_LENGTH + '\0').
    inline constexpr size_t MAX_LENGTH = 31;

    // 1..MAX_LENGTH printable ASCII without spaces, e.g. "1.2.0", "0.4.1-beta", "2026.11.09".
    inline bool IsValid(const std::string& version) {
        if (version.empty() || version.size() > MAX_LENGTH) {
            return false;
        }
        for (char c : version) {
            const unsigned char uc = static_cast<unsigned char>(c);
            if (uc <= ' ' || uc >= 0x7F) {
                return false;
            }
        }
        return true;
    }

    namespace detail
    {
        inline std::mutex& Mutex() { static std::mutex m; return m; }
        inline std::string& Value() { static std::string v; return v; }
    }

    // Invalid strings are rejected with an error and leave the previous version in place.
    inline bool Set(const std::string& version) {
        if (!IsValid(version)) {
            Debug::Error("GameVersion") << "Invalid game version '" << version << "': 1-" << MAX_LENGTH
                << " printable characters without spaces\n";
            return false;
        }
        {
            std::lock_guard<std::mutex> lock(detail::Mutex());
            detail::Value() = version;
        }
        Debug::Info("GameVersion") << "Game version " << version << "\n";
        return true;
    }

    // "major.minor.patch" plus an optional "-suffix" (e.g. Set(1, 2, 0, "beta") -> "1.2.0-beta").
    inline bool Set(int major, int minor, int patch, const std::string& suffix = "") {
        std::string version = std::to_string(major) + "." + std::to_string(minor) + "." + std::to_string(patch);
        if (!suffix.empty()) {
            version += "-" + suffix;
        }
        return Set(version);
    }

    // Copy, so it's safe to read from any thread. Empty when the game never set one.
    inline std::string Get() {
        std::lock_guard<std::mutex> lock(detail::Mutex());
        return detail::Value();
    }

    inline bool IsSet() { return !Get().empty(); }

    // Exact match; an unset version on either side is compatible with anything.
    inline bool IsCompatible(const std::string& a, const std::string& b) {
        return a.empty() || b.empty() || a == b;
    }

    inline bool IsCompatibleWithLocal(const std::string& other) {
        return IsCompatible(Get(), other);
    }
}
