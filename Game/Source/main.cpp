#include <iostream>
#include <memory>
#include <string>
#include <cstdlib>
#include <chrono>

#include "Client-Server/Server.hpp"  // Include for RunServer
#include "Client-Server/OnlineClient.hpp"  // Include for RunClient
#include "game/asteroids.hpp"        // Include for game logic

#include "Utils/Debug/Debug.hpp"

void PrintHelp() {
    std::cout << "Usage:\n"
        << "  --host                     Run as server (host) on the given port.\n"
        << "  --port <port>              Set the port number (default: 12345).\n"
        << "  --connect <host:port>      Connect to a server at the specified host and port.\n"
        << "  --id <client_id>          Specify a custom client ID.\n"
        << "  --help                     Show this help message.\n"
        << "\nExamples:\n"
        << "  Start server on default port:\n"
        << "    ./game --host\n\n"
        << "  Start server on custom port:\n"
        << "    ./game --host --port 5555\n\n"
        << "  Connect to server:\n"
        << "    ./game --connect 127.0.0.1:5555 --id player1\n";
}

int main(int argc, char** argv) {

    bool isHost = false;
    std::string connectTo;
    std::string customClientId = "";
    uint16_t port = 12345;

    // Parse arguments
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--help") {
            PrintHelp();
            return 0;
        }
        if (a == "--host") isHost = true;
        else if (a == "--port" && i + 1 < argc) port = static_cast<uint16_t>(std::atoi(argv[++i]));
        else if (a == "--connect" && i + 1 < argc) connectTo = argv[++i];
        else if (a == "--id" && i + 1 < argc) customClientId = argv[++i];
    }

    std::unique_ptr<IGameLogic> gameLogic = std::make_unique<AsteroidShooterGame>();
    std::unique_ptr<IGameRenderer> gameRenderer = std::make_unique<AsteroidShooterGameRenderer>();

    if (isHost) {
        ServerConfig config(port);
        config.allowReconnection = true;
        config.allowMidGameJoin = true;
        config.requireClientId = false;
        config.stopOnBelowMin = false;
        config.reconnectionTimeout = std::chrono::seconds(0);

        Debug::Initialize("AsteroidsServer");

        Server server(std::move(gameLogic), config);
        return server.RunServer();
    }
    else {
        if (connectTo.empty()) {
            std::cerr << "Error: Client must use --connect host:port\n";
            std::cerr << "Use --help to see usage.\n";
            return 1;
        }
        size_t colon = connectTo.find(':');
        std::string hoststr = connectTo.substr(0, colon);
        uint16_t p = port;
        if (colon != std::string::npos) {
            p = static_cast<uint16_t>(std::atoi(connectTo.substr(colon + 1).c_str()));
        }

		Debug::Initialize("AsteroidsClient");

        OnlineClient client(std::move(gameLogic), std::move(gameRenderer), customClientId);
        return client.RunClient(hoststr, p);
    }
}
