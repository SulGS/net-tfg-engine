#include "MatchmakingService.hpp"

#include <algorithm>

#include "Utils/Debug/Debug.hpp"
#include "Utils/PlayerKey.hpp"

using json = nlohmann::json;
namespace Protocol = MatchmakingProtocol;

namespace
{
    ApiResponse Error(int status, const std::string& message) {
        return { status, json{ { "error", message } } };
    }

    std::string JoinNames(const std::vector<std::string>& names, const char* separator) {
        std::string joined;
        for (const auto& name : names) {
            if (!joined.empty()) {
                joined += separator;
            }
            joined += name;
        }
        return joined;
    }
}

MatchmakingService::MatchmakingService(MatchmakerConfig config)
    : config_(std::move(config))
    , rng_(std::random_device{}())
{
    for (uint32_t port = config_.gamePortMin; port <= config_.gamePortMax; ++port) {
        freePorts_.insert(static_cast<uint16_t>(port));
    }
}

MatchmakingService::~MatchmakingService() {
    Shutdown();
}

ApiResponse MatchmakingService::Enqueue(const std::string& name, const std::string& key, int version) {
    if (version != Protocol::VERSION) {
        return Error(426, "Versión del juego incompatible con el matchmaking");
    }
    if (!Protocol::IsValidPlayerName(name)) {
        return Error(400, "Nickname inválido: 1-" + std::to_string(Protocol::MAX_NAME_LENGTH)
            + " letras, números, '-' o '_'");
    }
    if (!PlayerKey::IsValid(key)) {
        return Error(400, "Clave de jugador inválida");
    }

    std::lock_guard<std::mutex> lock(mutex_);

    Ticket ticket;
    ticket.id = RandomHex(16);
    ticket.name = name;
    ticket.key = key;
    ticket.lastSeen = Clock::now();

    auto active = activePlayers_.find(name);
    if (active != activePlayers_.end()) {
        const Match& match = matches_.at(active->second);
        size_t slot = std::find(match.names.begin(), match.names.end(), name) - match.names.begin();

        if (match.keys[slot] != key) {
            Debug::Info("Matchmaker") << "Rejected " << name << ": nickname in use in match " << match.id << "\n";
            return Error(409, "El nickname " + name + " ya está en una partida en curso");
        }

        // The game server accepts the same client id again as a reconnection.
        ticket.matchId = match.id;
        ticket.reconnect = true;
        tickets_[ticket.id] = ticket;

        Debug::Info("Matchmaker") << name << " reconnecting to match " << match.id
            << " on port " << match.port << "\n";
        return { 200, json{ { "ticket", ticket.id } } };
    }

    tickets_[ticket.id] = ticket;
    queue_.push_back(ticket.id);

    Debug::Info("Matchmaker") << "Queued " << name << " (" << queue_.size() << " in queue)\n";

    return { 200, json{ { "ticket", ticket.id } } };
}

ApiResponse MatchmakingService::Poll(const std::string& ticketId, const std::string& localAddress) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = tickets_.find(ticketId);
    if (it == tickets_.end()) {
        return Error(404, "Ticket desconocido");
    }

    Ticket& ticket = it->second;
    ticket.lastSeen = Clock::now();

    if (!ticket.error.empty()) {
        json body = { { "status", Protocol::Status::FAILURE }, { "error", ticket.error } };
        tickets_.erase(it);
        return { 200, body };
    }

    if (ticket.matchId < 0) {
        // Distinct names, since repeated nicknames can't be matched together.
        std::set<std::string> names;
        for (const auto& queuedId : queue_) {
            names.insert(tickets_.at(queuedId).name);
        }
        int queued = static_cast<int>(std::min<size_t>(names.size(), config_.playersPerMatch));
        return { 200, json{
            { "status", Protocol::Status::WAITING },
            { "queued", queued },
            { "needed", config_.playersPerMatch } } };
    }

    auto matchIt = matches_.find(ticket.matchId);
    if (matchIt == matches_.end()) {
        tickets_.erase(it);
        return { 200, json{ { "status", Protocol::Status::FAILURE }, { "error", "La partida ya no existe" } } };
    }

    const Match& match = matchIt->second;
    if (!match.ready) {
        return { 200, json{ { "status", Protocol::Status::STARTING } } };
    }

    return { 200, json{
        { "status", Protocol::Status::READY },
        { "ip", config_.publicIp.empty() ? localAddress : config_.publicIp },
        { "port", match.port },
        { "reconnect", ticket.reconnect } } };
}

ApiResponse MatchmakingService::Cancel(const std::string& ticketId) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = tickets_.find(ticketId);
    if (it == tickets_.end()) {
        return Error(404, "Ticket desconocido");
    }

    // A ticket already in a match just stops being tracked; the game server's
    // join timeout takes care of the player who never shows up.
    if (it->second.matchId < 0) {
        RemoveFromQueue(ticketId);
    }
    Debug::Info("Matchmaker") << it->second.name << " cancelled the search\n";
    tickets_.erase(it);

    return { 204, json() };
}

ApiResponse MatchmakingService::ServerReady(int matchId, const std::string& token) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = matches_.find(matchId);
    if (it == matches_.end() || it->second.token != token) {
        return Error(403, "Partida o token inválidos");
    }

    it->second.ready = true;
    Debug::Info("Matchmaker") << "Match " << matchId << " ready on port " << it->second.port << "\n";
    return { 200, json{ { "ok", true } } };
}

ApiResponse MatchmakingService::ServerEnded(int matchId, const std::string& token) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = matches_.find(matchId);
    if (it == matches_.end() || it->second.token != token) {
        return Error(403, "Partida o token inválidos");
    }

    // Nothing left to reconnect to. The port is released once the process actually exits (WatchMatches).
    ReleasePlayers(it->second);
    Debug::Info("Matchmaker") << "Match " << matchId << " ended\n";
    return { 200, json{ { "ok", true } } };
}

ApiResponse MatchmakingService::Status() {
    std::lock_guard<std::mutex> lock(mutex_);

    json matches = json::array();
    for (const auto& [id, match] : matches_) {
        matches.push_back({
            { "id", id },
            { "port", match.port },
            { "ready", match.ready },
            { "players", match.names } });
    }

    return { 200, json{
        { "queued", queue_.size() },
        { "playersInMatches", activePlayers_.size() },
        { "matches", matches },
        { "freePorts", freePorts_.size() } } };
}

void MatchmakingService::Tick() {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto now = Clock::now();

    ExpireTickets(now);
    WatchMatches(now);
    FormMatches(now);
}

void MatchmakingService::Shutdown() {
    std::lock_guard<std::mutex> lock(mutex_);

    for (auto& [id, match] : matches_) {
        if (match.process && match.process->IsRunning()) {
            Debug::Info("Matchmaker") << "Stopping game server of match " << id << "\n";
            match.process->Kill();
        }
        freePorts_.insert(match.port);
    }
    matches_.clear();
    activePlayers_.clear();
}

void MatchmakingService::ExpireTickets(Clock::time_point now) {
    const auto timeout = std::chrono::seconds(config_.ticketTimeoutS);

    for (auto it = tickets_.begin(); it != tickets_.end();) {
        if (now - it->second.lastSeen <= timeout) {
            ++it;
            continue;
        }

        if (it->second.matchId < 0) {
            RemoveFromQueue(it->first);
            Debug::Info("Matchmaker") << it->second.name << " stopped polling, removed from queue\n";
        }
        it = tickets_.erase(it);
    }
}

void MatchmakingService::WatchMatches(Clock::time_point now) {
    const auto readyTimeout = std::chrono::seconds(config_.serverReadyTimeoutS);

    for (auto it = matches_.begin(); it != matches_.end();) {
        Match& match = it->second;

        if (!match.process->IsRunning()) {
            Debug::Info("Matchmaker") << "Game server of match " << match.id << " exited with code "
                << match.process->ExitCode() << ", port " << match.port << " released\n";
            if (!match.ready) {
                FailMatchTickets(match, "El servidor de partida no pudo arrancar");
            }
            ReleasePlayers(match);
            freePorts_.insert(match.port);
            it = matches_.erase(it);
            continue;
        }

        if (!match.ready && now - match.launchedAt > readyTimeout) {
            Debug::Error("Matchmaker") << "Game server of match " << match.id << " did not report ready in "
                << config_.serverReadyTimeoutS << "s, killing it\n";
            match.process->Kill();
            FailMatchTickets(match, "El servidor de partida no respondió");
            ReleasePlayers(match);
            freePorts_.insert(match.port);
            it = matches_.erase(it);
            continue;
        }

        ++it;
    }
}

void MatchmakingService::FormMatches(Clock::time_point now) {
    while (true) {
        // First come, first served, skipping repeated nicknames: the name is the
        // client id on the game server, and two equal ids would kick each other out.
        std::vector<std::string> group;
        std::set<std::string> names;
        for (const auto& ticketId : queue_) {
            const Ticket& ticket = tickets_.at(ticketId);
            if (names.insert(ticket.name).second) {
                group.push_back(ticketId);
                if (group.size() == static_cast<size_t>(config_.playersPerMatch)) {
                    break;
                }
            }
        }

        if (group.size() < static_cast<size_t>(config_.playersPerMatch)) {
            return;
        }

        if (freePorts_.empty()) {
            if (!warnedNoPorts_) {
                Debug::Warning("Matchmaker") << "No free game server ports, players stay queued\n";
                warnedNoPorts_ = true;
            }
            return;
        }
        warnedNoPorts_ = false;

        for (const auto& ticketId : group) {
            RemoveFromQueue(ticketId);
        }
        LaunchMatch(group, now);
    }
}

void MatchmakingService::LaunchMatch(const std::vector<std::string>& ticketIds, Clock::time_point now) {
    Match match;
    match.id = nextMatchId_++;
    match.token = RandomHex(16);
    match.port = *freePorts_.begin();
    match.ticketIds = ticketIds;
    match.launchedAt = now;
    for (const auto& ticketId : ticketIds) {
        match.names.push_back(tickets_.at(ticketId).name);
        match.keys.push_back(tickets_.at(ticketId).key);
    }

    // Game servers run on this machine, so they always reach us through loopback
    // unless we are bound to one specific interface.
    const std::string reportHost = config_.bindAddress == "0.0.0.0" ? "127.0.0.1" : config_.bindAddress;

    // name:key pairs, so the game server only accepts each nickname from its own install.
    std::vector<std::string> allowed;
    for (size_t i = 0; i < match.names.size(); ++i) {
        allowed.push_back(match.names[i] + ":" + match.keys[i]);
    }

    std::vector<std::string> args = {
        "--port", std::to_string(match.port),
        "--players", std::to_string(config_.playersPerMatch),
        "--allowed", JoinNames(allowed, ","),
        "--join-timeout", std::to_string(config_.joinTimeoutS),
        "--empty-timeout", std::to_string(config_.emptyTimeoutS),
        "--matchmaker", reportHost + ":" + std::to_string(config_.httpPort),
        "--match-id", std::to_string(match.id),
        "--token", match.token
    };

    // No player names here: the window is visible to anyone looking at the host.
    const std::string title = "GameServer - Match " + std::to_string(match.id)
        + " - " + std::to_string(match.names.size()) + " players - UDP " + std::to_string(match.port);

    std::string error;
    match.process = ChildProcess::Launch(config_.gameServerPath, args,
        config_.gameServerPath.parent_path(), title, error);

    if (!match.process) {
        Debug::Error("Matchmaker") << "Could not launch game server for match " << match.id << ": " << error << "\n";
        FailMatchTickets(match, "No se pudo iniciar el servidor de partida");
        return;
    }

    freePorts_.erase(match.port);
    for (const auto& ticketId : ticketIds) {
        tickets_.at(ticketId).matchId = match.id;
    }
    for (const auto& name : match.names) {
        activePlayers_[name] = match.id;
    }

    // Anyone else still queued under one of these nicknames can't use it while this
    // match runs (Enqueue would answer 409 for them now).
    for (const auto& ticketId : std::vector<std::string>(queue_.begin(), queue_.end())) {
        Ticket& ticket = tickets_.at(ticketId);
        if (activePlayers_.count(ticket.name)) {
            ticket.error = "El nickname " + ticket.name + " ya está en una partida en curso";
            RemoveFromQueue(ticketId);
        }
    }

    Debug::Info("Matchmaker") << "Match " << match.id << " created on port " << match.port
        << " (pid " << match.process->Pid() << ") with " << match.names.size() << " players:\n";
    for (size_t i = 0; i < match.names.size(); ++i) {
        Debug::Info("Matchmaker") << "  Player " << (i + 1) << ": " << match.names[i] << "\n";
    }

    matches_.emplace(match.id, std::move(match));
}

void MatchmakingService::FailMatchTickets(const Match& match, const std::string& error) {
    for (const auto& ticketId : match.ticketIds) {
        auto it = tickets_.find(ticketId);
        if (it != tickets_.end()) {
            it->second.matchId = -1;
            it->second.error = error;
        }
    }
}

void MatchmakingService::ReleasePlayers(const Match& match) {
    for (const auto& name : match.names) {
        auto it = activePlayers_.find(name);
        if (it != activePlayers_.end() && it->second == match.id) {
            activePlayers_.erase(it);
        }
    }
}

void MatchmakingService::RemoveFromQueue(const std::string& ticketId) {
    queue_.erase(std::remove(queue_.begin(), queue_.end(), ticketId), queue_.end());
}

std::string MatchmakingService::RandomHex(size_t bytes) {
    static const char digits[] = "0123456789abcdef";
    std::uniform_int_distribution<int> byte(0, 255);

    std::string hex;
    hex.reserve(bytes * 2);
    for (size_t i = 0; i < bytes; ++i) {
        int b = byte(rng_);
        hex.push_back(digits[b >> 4]);
        hex.push_back(digits[b & 0x0F]);
    }
    return hex;
}
