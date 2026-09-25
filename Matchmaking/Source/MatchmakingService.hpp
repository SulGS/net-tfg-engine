#pragma once

#include <chrono>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <set>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "ChildProcess.hpp"
#include "Matchmaking/MatchmakingProtocol.hpp"

struct MatchmakerConfig {
    std::string bindAddress = "0.0.0.0";
    uint16_t httpPort = MatchmakingProtocol::DEFAULT_HTTP_PORT;

    // IP handed to clients. Empty = the local address the client reached the matchmaker on,
    // which is right for LAN/localhost; set it when running behind NAT.
    std::string publicIp;

    // UDP ports for game servers, one per running match.
    uint16_t gamePortMin = 27100;
    uint16_t gamePortMax = 27199;

    int playersPerMatch = 2;

    std::filesystem::path gameServerPath;

    int serverReadyTimeoutS = 15;  // launch -> /server/ready
    int joinTimeoutS = 60;         // passed to GameServer --join-timeout
    int emptyTimeoutS = 30;        // passed to GameServer --empty-timeout
    int ticketTimeoutS = MatchmakingProtocol::TICKET_TIMEOUT_S;
};

struct ApiResponse {
    int status = 200;
    nlohmann::json body;
};

// Queue + running matches. Every public method is thread-safe: HTTP handlers run on
// httplib's worker pool while Tick() runs on the management thread.
class MatchmakingService {
public:
    explicit MatchmakingService(MatchmakerConfig config);
    ~MatchmakingService();

    // Queues `name`, or sends it straight back to its running match when `key` matches
    // the one it was matched with (reconnection).
    ApiResponse Enqueue(const std::string& name, const std::string& key, int version);
    ApiResponse Poll(const std::string& ticketId, const std::string& localAddress);
    ApiResponse Cancel(const std::string& ticketId);

    ApiResponse ServerReady(int matchId, const std::string& token);
    ApiResponse ServerEnded(int matchId, const std::string& token);

    ApiResponse Status();

    // Expires tickets, watches game server processes and forms new matches.
    void Tick();

    // Kills every running game server.
    void Shutdown();

private:
    using Clock = std::chrono::steady_clock;

    struct Ticket {
        std::string id;
        std::string name;
        std::string key;
        Clock::time_point lastSeen;
        int matchId = -1;
        bool reconnect = false;  // sent back to a match it was already in
        std::string error;       // non-empty = failed
    };

    struct Match {
        int id = 0;
        std::string token;
        uint16_t port = 0;
        std::vector<std::string> ticketIds;
        std::vector<std::string> names;
        std::vector<std::string> keys;  // parallel to names
        std::unique_ptr<ChildProcess> process;
        Clock::time_point launchedAt;
        bool ready = false;
    };

    void ExpireTickets(Clock::time_point now);
    void WatchMatches(Clock::time_point now);
    void FormMatches(Clock::time_point now);
    void LaunchMatch(const std::vector<std::string>& ticketIds, Clock::time_point now);
    void FailMatchTickets(const Match& match, const std::string& error);
    void ReleasePlayers(const Match& match);
    void RemoveFromQueue(const std::string& ticketId);
    std::string RandomHex(size_t bytes);

    MatchmakerConfig config_;

    std::mutex mutex_;
    std::map<std::string, Ticket> tickets_;
    std::deque<std::string> queue_;
    std::map<int, Match> matches_;
    // Nickname -> match it plays in, from launch until the match ends. Lets a player
    // who dropped out ask for a match again and land back in the same one.
    std::map<std::string, int> activePlayers_;
    std::set<uint16_t> freePorts_;
    int nextMatchId_ = 1;
    std::mt19937_64 rng_;
    bool warnedNoPorts_ = false;
};
