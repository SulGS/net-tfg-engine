#pragma once

#include <string>

class Client {
public:
	bool isOfflineClient = false;

	virtual int RunClient(const std::string& hostStr = "0.0.0.0", uint16_t port = 0, const std::string& customClientId = "") = 0;
};