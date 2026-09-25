#pragma once

#include <cstddef>
#include <string>

// Random secret per nickname (one file per nickname in the user data folder, see ForName) tying a nickname to the
// install matched with it: only that install is sent back to a running match and accepted by the game server under
// that nickname.
namespace PlayerKey
{
    inline constexpr size_t LENGTH = 32;  // hex chars

    inline bool IsValid(const std::string& key) {
        if (key.size() != LENGTH) {
            return false;
        }
        for (char c : key) {
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
                return false;
            }
        }
        return true;
    }

    // Comparison whose time doesn't depend on where the keys differ.
    inline bool Equals(const std::string& a, const std::string& b) {
        if (a.size() != b.size()) {
            return false;
        }
        unsigned char diff = 0;
        for (size_t i = 0; i < a.size(); ++i) {
            diff |= static_cast<unsigned char>(a[i] ^ b[i]);
        }
        return diff == 0;
    }

    // Key for `name`, read from player_keys/<name>.key or created there on first use, so
    // several game instances on the same PC with different nicknames get different keys, and
    // a game restarted after a crash with the same nickname gets its key back to reconnect.
    // Cached per name. Must run after Debug::Initialize, which sets the per-user folder.
    const std::string& ForName(const std::string& name);

    // New random key that isn't saved anywhere (clients without a nickname).
    std::string Generate();
}
