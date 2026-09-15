#include <iostream>
#include <memory>
#include <string>
#include <cstdlib>
#include <chrono>

#include "Client-Server/OnlineClient.hpp"
#include "Client-Server/OfflineClient.hpp"

#include "game/asteroids.hpp"
#include "game/menu.hpp"
#include "game/settings_menu.hpp"


#include "NetTFG_Engine.hpp"

#include "Utils/Debug/Debug.hpp"


#include <filesystem>

int main(int argc, char** argv) {

	std::cout << std::filesystem::current_path() << '\n';


	std::unique_ptr<IGameLogic> gameLogic = std::make_unique<AsteroidShooterGame>();
	std::unique_ptr<IGameRenderer> gameRenderer = std::make_unique<AsteroidShooterGameRenderer>();

	gameRenderer->LinkGameLogic(gameLogic.get());

	auto& engine = NetTFG_Engine::Get();

#if defined(_DEBUG) || defined(DEBUG)
	Debug::Initialize("AsteroidsClient", true);
#else
	Debug::Initialize("AsteroidsClient", false);
#endif



	engine.RegisterClient(1, new OnlineClient(std::move(gameLogic), std::move(gameRenderer), "online_level.bin"));

	std::unique_ptr<IGameLogic> menuLogic = std::make_unique<StartScreenGame>();
	std::unique_ptr<IGameRenderer> menuRenderer = std::make_unique<StartScreenGameRenderer>();

	menuRenderer->LinkGameLogic(menuLogic.get());

	engine.RegisterClient(MENU_SCENE_ID, new OfflineClient(std::move(menuLogic), std::move(menuRenderer), "menu.bin"));

	// Settings scene uses OfflineClient too, since it doesn't connect anywhere.
	std::unique_ptr<IGameLogic> settingsLogic = std::make_unique<SettingsScreenGame>();
	std::unique_ptr<IGameRenderer> settingsRenderer = std::make_unique<SettingsScreenGameRenderer>();

	settingsRenderer->LinkGameLogic(settingsLogic.get());

	engine.RegisterClient(SETTINGS_SCENE_ID, new OfflineClient(std::move(settingsLogic), std::move(settingsRenderer), "settings.bin"));

	engine.ActivateClient(MENU_SCENE_ID);
	engine.Start(800, 600, "Asteroids");

	Debug::Shutdown();

	return 0;

}