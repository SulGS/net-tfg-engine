#include <iostream>
#include <memory>
#include <string>
#include <cstdlib>
#include <chrono>
#include <optional>
#include <sstream>

#include "Client-Server/Server.hpp"
#include "game/asteroids.hpp"

#include "Matchmaking/MatchmakingClient.hpp"
#include "Matchmaking/MatchmakerReporter.hpp"

#include "Utils/Debug/Debug.hpp"
#include "Utils/PlayerKey.hpp"

void PrintHelp() {
    std::cout << "Usage:\n"
        << "  --port <port>              Set the port number (default: 12345).\n"
        << "  --players <n>              Players needed to start the match (default: 2, max: " << NUM_PLAYERS << ").\n"
        << "  --allowed <id:key,...>     Only these client ids may join, each with its player key.\n"
        << "                             With --no-key-check the keys can be left out (--allowed a,b).\n"
        << "  --no-key-check             Accept clients without checking their player key. For\n"
        << "                             direct connections during development only.\n"
        << "  --join-timeout <seconds>   Stop if the players haven't joined in time (default: 0 = never).\n"
        << "  --empty-timeout <seconds>  Stop when nobody has been connected for this long (default: 0 = never).\n"
        << "  --help                     Show this help message.\n"
        << "\nThe player key check is on by default: every client must send the key given for\n"
        << "its id in --allowed, so without --no-key-check the server needs --allowed id:key.\n"
        << "\nSet by the matchmaking server when it launches a match:\n"
        << "  --matchmaker <host:port>   Matchmaker to report ready/ended to.\n"
        << "  --match-id <id>            Match id assigned by the matchmaker.\n"
        << "  --token <token>            Secret that authenticates the reports.\n"
        << "\nExamples:\n"
        << "  Development server, direct connections without keys:\n"
        << "    ./GameServer --port 5555 --players 2 --no-key-check\n";
}

static std::vector<std::string> SplitCommaList(const std::string& text) {
    std::vector<std::string> items;
    std::stringstream ss(text);
    std::string item;
    while (std::getline(ss, item, ',')) {
        if (!item.empty()) {
            items.push_back(item);
        }
    }
    return items;
}

int main(int argc, char** argv) {

    uint16_t port = 12345;
    size_t players = 2;
    std::vector<std::string> allowedIds;
    std::map<std::string, std::string> playerKeys;
    bool checkPlayerKeys = true;
    int joinTimeout = 0;
    int emptyTimeout = 0;

    std::optional<MatchmakerAddress> matchmaker;
    int matchId = -1;
    std::string token;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--help") {
            PrintHelp();
            return 0;
        }

        if (a == "--port" && i + 1 < argc) port = static_cast<uint16_t>(std::atoi(argv[++i]));
        else if (a == "--players" && i + 1 < argc) players = static_cast<size_t>(std::max(1, std::atoi(argv[++i])));
        else if (a == "--no-key-check") checkPlayerKeys = false;
        else if (a == "--allowed" && i + 1 < argc) {
            // Each entry is "id" or "id:key".
            for (const auto& entry : SplitCommaList(argv[++i])) {
                size_t colon = entry.find(':');
                std::string id = entry.substr(0, colon);
                allowedIds.push_back(id);
                if (colon != std::string::npos) {
                    std::string key = entry.substr(colon + 1);
                    if (!PlayerKey::IsValid(key)) {
                        std::cerr << "Invalid player key for '" << id << "' in --allowed\n";
                        return 1;
                    }
                    playerKeys[id] = key;
                }
            }
        }
        else if (a == "--join-timeout" && i + 1 < argc) joinTimeout = std::max(0, std::atoi(argv[++i]));
        else if (a == "--empty-timeout" && i + 1 < argc) emptyTimeout = std::max(0, std::atoi(argv[++i]));
        else if (a == "--match-id" && i + 1 < argc) matchId = std::atoi(argv[++i]);
        else if (a == "--token" && i + 1 < argc) token = argv[++i];
        else if (a == "--matchmaker" && i + 1 < argc) {
            MatchmakerAddress address;
            if (ParseHostPort(argv[++i], address)) {
                matchmaker = address;
            }
            else {
                std::cerr << "Invalid --matchmaker value, expected host:port\n";
                return 1;
            }
        }
    }

    // The game state holds a fixed number of ship slots.
    if (players > static_cast<size_t>(NUM_PLAYERS)) {
        std::cerr << "--players " << players << " exceeds the " << NUM_PLAYERS << " players the game supports\n";
        return 1;
    }

    // With the check on, a client whose id has no key could never join: fail now instead.
    if (checkPlayerKeys) {
        bool missingKey = allowedIds.empty();
        for (const auto& id : allowedIds) {
            if (!playerKeys.count(id)) {
                missingKey = true;
            }
        }
        if (missingKey) {
            std::cerr << "The player key check is on, so every player needs a key: use --allowed id:key,...\n"
                << "For direct connections during development, add --no-key-check.\n";
            return 1;
        }
    }

    std::unique_ptr<IGameLogic> gameLogic = std::make_unique<AsteroidShooterGame>();
    std::unique_ptr<IGameRenderer> gameRenderer = std::make_unique<AsteroidShooterGameRenderer>();

    ServerConfig config(port);
	config.minPlayers = players;
    config.maxPlayers = players;
    config.allowReconnection = true;
    config.allowMidGameJoin = true;
    config.requireClientId = !allowedIds.empty();
    config.allowedClientIds = allowedIds;
    config.checkPlayerKeys = checkPlayerKeys;
    config.playerKeys = playerKeys;
    config.stopOnBelowMin = false;
    config.reconnectionTimeout = std::chrono::seconds(0);
    config.joinTimeout = std::chrono::seconds(joinTimeout);
    config.emptyTimeout = std::chrono::seconds(emptyTimeout);

    Debug::Initialize("AsteroidsServer", true);

    std::optional<MatchmakerReporter> reporter;
    if (matchmaker && matchId >= 0) {
        reporter.emplace(matchmaker->host, matchmaker->port, matchId, token);
        config.onListening = [&reporter]() { reporter->ReportReady(); };
        Debug::Info("Server") << "Launched by matchmaker " << matchmaker->host << ":" << matchmaker->port
            << " as match " << matchId << "\n";
    }

    Server server(std::move(gameLogic), config);
    int code = server.RunServer();

    if (reporter) {
        reporter->ReportEnded();
    }

    Debug::Shutdown();

    return code;
}
