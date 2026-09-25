// httplib first: it pulls in winsock2.h, which must precede any windows.h.
#include <httplib.h>
#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>

#include "MatchmakingService.hpp"
#include "Utils/Debug/Debug.hpp"
#include "Utils/UserDataPath.hpp"

using json = nlohmann::json;

namespace
{
    httplib::Server* g_server = nullptr;

    void OnSignal(int) {
        if (g_server) {
            g_server->stop();
        }
    }

    constexpr const char* CONFIG_FILE = "matchmaker.cfg";

    void PrintHelp() {
        std::cout << "Usage:\n"
            << "  --bind <address>            Interface to listen on (default: 0.0.0.0).\n"
            << "  --http-port <port>          HTTP port for clients (default: " << MatchmakingProtocol::DEFAULT_HTTP_PORT << ").\n"
            << "  --public-ip <ip>            IP given to clients (default: the one they reached us on).\n"
            << "  --port-range <min-max>      UDP ports for game servers (default: 27100-27199).\n"
            << "  --players <n>               Players per match (default: 2).\n"
            << "  --game-server <path>        GameServer executable (default: next to this one).\n"
            << "  --ready-timeout <seconds>   Max time for a game server to start (default: 15).\n"
            << "  --join-timeout <seconds>    Max time for matched players to join (default: 60).\n"
            << "  --empty-timeout <seconds>   Stop a match with nobody connected (default: 30).\n"
            << "  --help                      Show this help message.\n"
            << "\nThe same options (without --) can be set in " << CONFIG_FILE << ", in the per-user\n"
            << "data folder; it is created with the defaults on first run. Arguments override it.\n";
    }

    // Written on first run so the settings can be changed without touching the command line.
    constexpr const char* DEFAULT_CONFIG =
        "# Servidor de matchmaking. Formato: <clave> <valor>. Las lineas con # se ignoran.\n"
        "# Los argumentos --<clave> <valor> de la linea de comandos tienen prioridad.\n"
        "\n"
        "# Jugadores por partida (el juego admite hasta 12).\n"
        "players 2\n"
        "\n"
        "# Interfaz y puerto TCP en los que escucha el HTTP.\n"
        "bind 0.0.0.0\n"
        "http-port 27000\n"
        "\n"
        "# IP que se da a los clientes. Sin ella, la IP por la que llegaron (vale en local/LAN).\n"
        "# Necesaria detras de un router con NAT.\n"
        "# public-ip 203.0.113.25\n"
        "\n"
        "# Puertos UDP para los GameServers, uno por partida en curso.\n"
        "port-range 27100-27199\n"
        "\n"
        "# Ejecutable del GameServer. Por defecto, el que esta junto al matchmaker.\n"
        "# game-server C:\\ruta\\GameServer.exe\n"
        "\n"
        "# Tiempos en segundos: arranque del GameServer, entrada de los jugadores\n"
        "# y cierre de una partida que se queda sin nadie.\n"
        "ready-timeout 15\n"
        "join-timeout 60\n"
        "empty-timeout 30\n";

    bool ParseInt(const std::string& text, int min, int max, int& out) {
        try {
            size_t used = 0;
            int value = std::stoi(text, &used);
            if (used != text.size() || value < min || value > max) {
                return false;
            }
            out = value;
            return true;
        }
        catch (const std::exception&) {
            return false;
        }
    }

    bool ParsePortRange(const std::string& text, uint16_t& min, uint16_t& max) {
        size_t dash = text.find('-');
        int lo = 0;
        int hi = 0;
        if (dash == std::string::npos
            || !ParseInt(text.substr(0, dash), 1, 65535, lo)
            || !ParseInt(text.substr(dash + 1), 1, 65535, hi)
            || lo > hi) {
            return false;
        }
        min = static_cast<uint16_t>(lo);
        max = static_cast<uint16_t>(hi);
        return true;
    }

    // Single parser for the config file and the command line (key without "--").
    bool ApplyOption(const std::string& key, const std::string& value, MatchmakerConfig& config, std::string& error) {
        int number = 0;

        if (key == "bind") config.bindAddress = value;
        else if (key == "public-ip") config.publicIp = value;
        else if (key == "game-server") config.gameServerPath = std::filesystem::absolute(value);
        else if (key == "http-port" && ParseInt(value, 1, 65535, number)) config.httpPort = static_cast<uint16_t>(number);
        else if (key == "players" && ParseInt(value, 1, 64, number)) config.playersPerMatch = number;
        else if (key == "ready-timeout" && ParseInt(value, 1, 3600, number)) config.serverReadyTimeoutS = number;
        else if (key == "join-timeout" && ParseInt(value, 0, 3600, number)) config.joinTimeoutS = number;
        else if (key == "empty-timeout" && ParseInt(value, 0, 3600, number)) config.emptyTimeoutS = number;
        else if (key == "port-range" && ParsePortRange(value, config.gamePortMin, config.gamePortMax)) {}
        else {
            error = "invalid option '" + key + "' = '" + value + "'";
            return false;
        }
        return true;
    }

    std::string Trim(const std::string& text) {
        const char* spaces = " \t\r\n";
        size_t begin = text.find_first_not_of(spaces);
        if (begin == std::string::npos) {
            return "";
        }
        return text.substr(begin, text.find_last_not_of(spaces) - begin + 1);
    }

    // Reads `path` into `config`, creating it with DEFAULT_CONFIG when missing.
    bool LoadConfigFile(const std::string& path, MatchmakerConfig& config) {
        std::ifstream in(path);
        if (!in.is_open()) {
            std::ofstream out(path);
            out << DEFAULT_CONFIG;
            if (out.good()) {
                Debug::Info("Matchmaker") << "Created " << path << " with the default settings\n";
            }
            in.open(path);
            if (!in.is_open()) {
                return true;  // Nothing to read; defaults apply.
            }
        }

        std::string line;
        int lineNumber = 0;
        bool ok = true;
        while (std::getline(in, line)) {
            ++lineNumber;
            line = Trim(line);
            if (line.empty() || line[0] == '#') {
                continue;
            }

            size_t split = line.find_first_of(" \t");
            std::string key = line.substr(0, split);
            std::string value = split == std::string::npos ? "" : Trim(line.substr(split));

            std::string error;
            if (!ApplyOption(key, value, config, error)) {
                Debug::Error("Matchmaker") << path << ":" << lineNumber << ": " << error << "\n";
                ok = false;
            }
        }
        return ok;
    }

    void Reply(httplib::Response& res, const ApiResponse& api) {
        res.status = api.status;
        if (!api.body.is_null()) {
            res.set_content(api.body.dump(), "application/json");
        }
    }

    void ReplyBadRequest(httplib::Response& res, const std::string& message) {
        Reply(res, { 400, json{ { "error", message } } });
    }

    // Common shape of /server/ready and /server/ended.
    template <typename Handler>
    void HandleServerReport(const httplib::Request& req, httplib::Response& res, Handler handler) {
        try {
            json body = json::parse(req.body);
            Reply(res, handler(body.at("matchId").get<int>(), body.at("token").get<std::string>()));
        }
        catch (const json::exception&) {
            ReplyBadRequest(res, "Se esperaba {\"matchId\", \"token\"}");
        }
    }
}

int main(int argc, char** argv) {
    MatchmakerConfig config;
    config.gameServerPath = CurrentExecutableDirectory() /
#ifdef _WIN32
        "GameServer.exe";
#else
        "GameServer";
#endif

    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--help") {
            PrintHelp();
            return 0;
        }
    }

    // Before reading the config file: it sets the per-user folder the file lives in.
    Debug::Initialize("Matchmaker", true);

    const std::string configPath = UserDataPath::File(CONFIG_FILE);
    if (!LoadConfigFile(configPath, config)) {
        Debug::Shutdown();
        return 1;
    }

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        std::string error;
        if (a.rfind("--", 0) != 0 || i + 1 >= argc) {
            error = "unknown or incomplete argument '" + a + "'";
        }
        else {
            ApplyOption(a.substr(2), argv[++i], config, error);
        }

        if (!error.empty()) {
            Debug::Error("Matchmaker") << error << " (see --help)\n";
            Debug::Shutdown();
            return 1;
        }
    }

    if (!std::filesystem::exists(config.gameServerPath)) {
        Debug::Error("Matchmaker") << "Game server not found at " << config.gameServerPath.string()
            << " (set game-server in " << configPath << " or use --game-server)\n";
        Debug::Shutdown();
        return 1;
    }

    MatchmakingService service(config);
    httplib::Server server;
    server.set_payload_max_length(4096);
    // Clients open one short request at a time; a long keep-alive only delays Ctrl+C.
    server.set_keep_alive_timeout(1);

    server.Post("/queue", [&service](const httplib::Request& req, httplib::Response& res) {
        try {
            json body = json::parse(req.body);
            // "key" is optional here so a client from before it existed gets the clear 426 version error.
            Reply(res, service.Enqueue(body.at("name").get<std::string>(),
                body.value("key", std::string()), body.at("version").get<int>()));
        }
        catch (const json::exception&) {
            ReplyBadRequest(res, "Se esperaba {\"name\", \"key\", \"version\"}");
        }
    });

    server.Get(R"(/queue/([0-9a-f]+))", [&service](const httplib::Request& req, httplib::Response& res) {
        Reply(res, service.Poll(req.matches[1], req.local_addr));
    });

    server.Delete(R"(/queue/([0-9a-f]+))", [&service](const httplib::Request& req, httplib::Response& res) {
        Reply(res, service.Cancel(req.matches[1]));
    });

    server.Post("/server/ready", [&service](const httplib::Request& req, httplib::Response& res) {
        HandleServerReport(req, res, [&service](int id, const std::string& token) { return service.ServerReady(id, token); });
    });

    server.Post("/server/ended", [&service](const httplib::Request& req, httplib::Response& res) {
        HandleServerReport(req, res, [&service](int id, const std::string& token) { return service.ServerEnded(id, token); });
    });

    server.Get("/status", [&service](const httplib::Request&, httplib::Response& res) {
        Reply(res, service.Status());
    });

    // Management loop: expire tickets, watch game servers, form matches.
    std::mutex tickMutex;
    std::condition_variable tickCv;
    bool stopping = false;
    std::thread tickThread([&]() {
        std::unique_lock<std::mutex> lock(tickMutex);
        while (!tickCv.wait_for(lock, std::chrono::milliseconds(200), [&] { return stopping; })) {
            lock.unlock();
            service.Tick();
            lock.lock();
        }
    });

    g_server = &server;
    std::signal(SIGINT, OnSignal);
    std::signal(SIGTERM, OnSignal);

    Debug::Info("Matchmaker") << "Config: " << configPath << "\n";
    Debug::Info("Matchmaker") << "Listening on " << config.bindAddress << ":" << config.httpPort
        << " | " << config.playersPerMatch << " players per match"
        << " | game ports " << config.gamePortMin << "-" << config.gamePortMax
        << " | game server " << config.gameServerPath.string() << "\n";

    int code = 0;
    if (!server.listen(config.bindAddress, config.httpPort)) {
        Debug::Error("Matchmaker") << "Could not listen on " << config.bindAddress << ":" << config.httpPort << "\n";
        code = 1;
    }

    g_server = nullptr;
    {
        std::lock_guard<std::mutex> lock(tickMutex);
        stopping = true;
    }
    tickCv.notify_all();
    tickThread.join();

    service.Shutdown();
    Debug::Info("Matchmaker") << "Stopped\n";
    Debug::Shutdown();

    return code;
}
