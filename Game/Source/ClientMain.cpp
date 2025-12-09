#include <iostream>
#include <memory>
#include <string>
#include <cstdlib>
#include <chrono>

#include "Client-Server/OnlineClient.hpp"  // Include for RunClient
#include "Client-Server/OfflineClient.hpp"  // Include for RunClient

#include "game/asteroids.hpp"        // Include for game logic
#include "game/menu.hpp"

#include "OpenAL/AudioManager.hpp"
#include "NetTFG_Engine.hpp"

#include "Utils/Debug/Debug.hpp"

int main(int argc, char** argv) {

    std::string customClientId = "";

    // Parse arguments
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--id" && i + 1 < argc) customClientId = argv[++i];
    }

    std::unique_ptr<IGameLogic> gameLogic = std::make_unique<AsteroidShooterGame>();
    std::unique_ptr<IGameRenderer> gameRenderer = std::make_unique<AsteroidShooterGameRenderer>();

    auto& engine = NetTFG_Engine::Get();

    Debug::Initialize("AsteroidsClient");
	engine.RegisterClient(1, new OnlineClient(std::move(gameLogic), std::move(gameRenderer), customClientId));

	std::unique_ptr<IGameLogic> menuLogic = std::make_unique<StartScreenGame>();
	std::unique_ptr<IGameRenderer> menuRenderer = std::make_unique<StartScreenGameRenderer>();

	engine.RegisterClient(0, new OfflineClient(std::move(menuLogic), std::move(menuRenderer)));




    ClientWindow::startRenderThread(800, 600, "Asteroids");
    AudioManager::Start();
        


	engine.RequestClientSwitch(0);
    engine.Start();

    return 0;
    
}
