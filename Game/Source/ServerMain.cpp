#include <iostream>
#include <memory>
#include <string>
#include <cstdlib>
#include <chrono>

#include "Client-Server/Server.hpp"  
#include "game/asteroids.hpp"       

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

    uint16_t port = 12345;

    // Parse arguments
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--help") {
            PrintHelp();
            return 0;
        }
        
        if (a == "--port" && i + 1 < argc) port = static_cast<uint16_t>(std::atoi(argv[++i]));
    }

    std::unique_ptr<IGameLogic> gameLogic = std::make_unique<AsteroidShooterGame>();
    std::unique_ptr<IGameRenderer> gameRenderer = std::make_unique<AsteroidShooterGameRenderer>();

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
