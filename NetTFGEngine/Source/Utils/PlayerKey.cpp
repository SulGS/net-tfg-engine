#include "Utils/PlayerKey.hpp"
#include "Utils/UserDataPath.hpp"

#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <random>
#include <system_error>

namespace
{
    // Nicknames are case-sensitive but Windows file names aren't, so everything except
    // [a-z0-9-] is written as _XX (hex): "Saul" -> "_53aul".
    std::string FileNameFor(const std::string& name) {
        static const char digits[] = "0123456789abcdef";
        std::string file;
        for (char c : name) {
            unsigned char uc = static_cast<unsigned char>(c);
            if ((uc >= 'a' && uc <= 'z') || (uc >= '0' && uc <= '9') || c == '-') {
                file.push_back(c);
            }
            else {
                file.push_back('_');
                file.push_back(digits[uc >> 4]);
                file.push_back(digits[uc & 0xF]);
            }
        }
        return file + ".key";
    }

    // Falls back to the working directory if the per-user folder is unavailable.
    std::string KeyPathFor(const std::string& name) {
        std::filesystem::path dir = UserDataPath::Directory();
        if (!dir.empty()) {
            dir /= "player_keys";
            std::error_code ec;
            std::filesystem::create_directories(dir, ec);
            if (!ec) {
                return (dir / FileNameFor(name)).string();
            }
        }
        return FileNameFor(name);
    }

    std::string ReadOrCreateKey(const std::string& name) {
        const std::string path = KeyPathFor(name);

        std::string key;
        {
            std::ifstream in(path);
            in >> key;
        }
        if (PlayerKey::IsValid(key)) {
            return key;
        }

        key = PlayerKey::Generate();

        // If it can't be saved the key still works for this session; reconnecting after
        // restarting the game just won't be possible.
        std::ofstream out(path);
        out << key << "\n";
        return key;
    }
}

std::string PlayerKey::Generate() {
    static const char digits[] = "0123456789abcdef";
    std::random_device random;
    std::uniform_int_distribution<int> digit(0, 15);
    std::string key;
    for (size_t i = 0; i < LENGTH; ++i) {
        key.push_back(digits[digit(random)]);
    }
    return key;
}

const std::string& PlayerKey::ForName(const std::string& name) {
    // Cached so the matchmaker and the game server always get the same key in a session,
    // even when the file can't be written. Map nodes are stable, so the reference stays valid.
    static std::mutex mutex;
    static std::map<std::string, std::string> cache;

    std::lock_guard<std::mutex> lock(mutex);
    auto it = cache.find(name);
    if (it == cache.end()) {
        it = cache.emplace(name, ReadOrCreateKey(name)).first;
    }
    return it->second;
}
