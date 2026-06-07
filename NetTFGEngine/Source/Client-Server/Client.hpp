#pragma once

#include <string>
#include "ecs/ecs.hpp"
#include "ecs/ecs_gamelogic.hpp"
#include "netcode/valve_sockets_session.hpp"

class Client {
public:
	bool isOfflineClient = false;
	std::string binName = "Unknown";

	virtual ConnectionCode SetupClient(const std::string& hostStr = "0.0.0.0", uint16_t port = 0, const std::string& customClientId = "") = 0;
	virtual void TickClient() = 0;
	virtual void CloseClient() = 0;
	virtual EntityManager* GetEntityManager() = 0;
};