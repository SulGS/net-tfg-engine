#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

#include "Matchmaking/MatchmakingProtocol.hpp"

struct MatchmakerAddress {
    std::string host = "127.0.0.1";
    uint16_t port = MatchmakingProtocol::DEFAULT_HTTP_PORT;
};

// Parses "host:port". Leaves `out` untouched and returns false on malformed input.
bool ParseHostPort(const std::string& text, MatchmakerAddress& out);

// Reads matchmaking.cfg ("host <h>" / "port <p>" lines) from the per-user data folder.
// When the file is missing it is created with the defaults so the player can edit it.
// Must be called after Debug::Initialize, which sets the UserDataPath product folder.
MatchmakerAddress LoadMatchmakerAddress();

enum class MatchmakingState {
    Idle,
    Queuing,   // POST /queue in flight
    Waiting,   // in queue, waiting for more players
    Starting,  // match formed, game server launching
    Ready,     // ip/port available
    Failed     // see error
};

struct MatchmakingStatus {
    MatchmakingState state = MatchmakingState::Idle;
    int queued = 0;
    int needed = 0;
    std::string playerName;
    std::string ip;
    uint16_t port = 0;
    bool reconnecting = false;  // Ready: going back to a match this player was already in
    std::string error;

    bool IsSearching() const {
        return state == MatchmakingState::Queuing
            || state == MatchmakingState::Waiting
            || state == MatchmakingState::Starting;
    }
};

struct MatchmakingSearch;

// Talks HTTP to the matchmaker on a background thread; callers (render-thread UI) only read a status snapshot. Cancel()
// detaches the search at once and the old worker finishes alone (DELETE /queue/<ticket>), so a new one can start.
class MatchmakingClient {
public:
    ~MatchmakingClient();

    void SetServer(const MatchmakerAddress& address);
    MatchmakerAddress GetServer() const;

    // Starts a search for `playerName`. Returns false if one is already running.
    bool Start(const std::string& playerName);

    // Stops the current search; the status goes back to Idle at once.
    void Cancel();

    // Clears a finished search (Ready/Failed) back to Idle. No-op while searching.
    void Reset();

    MatchmakingStatus GetStatus() const;

private:
    mutable std::mutex mutex_;
    MatchmakerAddress server_;
    std::shared_ptr<MatchmakingSearch> search_;
};
