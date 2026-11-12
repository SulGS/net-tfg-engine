#pragma once

#include <string>
#include <vector>
#include "ecs/ecs.hpp"
#include "ecs/ecs_gamelogic.hpp"
#include "netcode/valve_sockets_session.hpp"

class Client {
public:
	bool isOfflineClient = false;
	std::string binName = "Unknown";

	// Keep this client's bin and its loaded assets across activations (AssetManager::setKeepResident), so going back
	// to it (e.g. the next match) doesn't decode and upload everything again.
	bool keepAssetsResident = false;
	// Meshes whose CPU half (glTF parse, image decode, mips) starts on a worker thread as soon as the activation loads
	// the bin, i.e. while an OnlineClient still waits for its match (AssetManager::prefetch).
	std::vector<std::string> prefetchMeshes;

	virtual ~Client() = default;

	virtual ConnectionCode SetupClient(const std::string& hostStr = "0.0.0.0", uint16_t port = 0, const std::string& customClientId = "") = 0;
	virtual void TickClient() = 0;
	virtual void CloseClient() = 0;
	virtual EntityManager* GetEntityManager() = 0;

	virtual EntityManager* GetRendererEntityManager() { return nullptr; }
};