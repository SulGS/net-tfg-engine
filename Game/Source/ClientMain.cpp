#include <iostream>
#include <memory>
#include <string>
#include <cstdlib>
#include <chrono>

#include "Client-Server/OnlineClient.hpp"  // Include for RunClient
#include "Client-Server/OfflineClient.hpp"  // Include for RunClient

#include "game/asteroids.hpp"        // Include for game logic
#include "game/menu.hpp"


#include "NetTFG_Engine.hpp"

#include "Utils/Debug/Debug.hpp"

int main(int argc, char** argv) {


    std::unique_ptr<IGameLogic> gameLogic = std::make_unique<AsteroidShooterGame>();
    std::unique_ptr<IGameRenderer> gameRenderer = std::make_unique<AsteroidShooterGameRenderer>();

	gameRenderer->LinkGameLogic(gameLogic.get());

    auto& engine = NetTFG_Engine::Get();

    Debug::Initialize("AsteroidsClient", false);
	engine.RegisterClient(1, new OnlineClient(std::move(gameLogic), std::move(gameRenderer)));

	std::unique_ptr<IGameLogic> menuLogic = std::make_unique<StartScreenGame>();
	std::unique_ptr<IGameRenderer> menuRenderer = std::make_unique<StartScreenGameRenderer>();

	menuRenderer->LinkGameLogic(menuLogic.get());

	engine.RegisterClient(0, new OfflineClient(std::move(menuLogic), std::move(menuRenderer)));

	engine.ActivateClient(0);
    engine.Start(800, 600, "Asteroids");

	Debug::Shutdown();

    return 0;
    
}
