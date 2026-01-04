#pragma once
#include <unordered_map>
#include <string>
#include <iostream>
#include <memory>

#include "Client-Server/ClientManager.hpp"
#include "Utils/Debug/Debug.hpp"

class NetTFG_Engine {
public:
    // ---- Singleton ----
    static NetTFG_Engine& Get() {
        static NetTFG_Engine instance;
        return instance;
    }

    // Prevent copy/move
    NetTFG_Engine(const NetTFG_Engine&) = delete;
    NetTFG_Engine& operator=(const NetTFG_Engine&) = delete;
    NetTFG_Engine(NetTFG_Engine&&) = delete;
    NetTFG_Engine& operator=(NetTFG_Engine&&) = delete;

    // ---- Register client using pointer ----
    // Engine takes ownership internally (via unique_ptr)
    void RegisterClient(int id, Client* client) {
        auto& mgr = ClientManager::Get();
        size_t index = mgr.AddClient(std::unique_ptr<Client>(client));
        clientIndexMap[id] = index;
    }

    // ---- Request a client switch WITH host/port ----
    void RequestClientSwitch(int id,
        const std::string& host,
        uint16_t port, const std::string& customClientId = "")
    {
        auto it = clientIndexMap.find(id);
        if (it == clientIndexMap.end()) return;

        pendingHost = host;
        pendingPort = port;
		pendingName = customClientId;
        hostAssigned = true;

        ClientManager::Get().RequestClientSwitch(it->second);
    }

    // ---- Request a client switch WITHOUT host/port ----
    void RequestClientSwitch(int id) {
        auto it = clientIndexMap.find(id);
        if (it == clientIndexMap.end()) return;

        hostAssigned = false;
        ClientManager::Get().RequestClientSwitch(it->second);
    }

    // ---- Main engine loop ----
    void Start() {
        running = true;

        while (running) {
            // Apply pending switch
            ClientManager::Get().ApplyPendingSwitch();

            // If no host assigned, use default
            if (!hostAssigned) {
                pendingHost = "0.0.0.0";
                pendingPort = 0;
				pendingName = "";
            }

            Client* client = ClientManager::Get().GetActiveClient();
            if (!client) {
                std::cerr << "[Engine] ERROR: No active client!\n";
                Stop();
                break;
            }

            // Run active client with stored host/port
            int exitCode = client->RunClient(pendingHost, pendingPort, pendingName);

			Debug::Info("NetTFG_Engine") << "Client exited with code " << exitCode << "\n";

            if (exitCode != 0)
                running = false;

            if(!ClientManager::Get().HasPendingSwitch())
				running = false;
        }
    }

	bool HasPendingSwitch() const {
		return ClientManager::Get().HasPendingSwitch();
	}

    void Stop() { running = false; }

private:
    NetTFG_Engine() = default;

private:
    bool running = false;
    std::unordered_map<int, size_t> clientIndexMap;

    // Host/port used for next active client
    std::string pendingHost = "0.0.0.0";
    uint16_t    pendingPort = 0;
    std::string pendingName = "";
    bool        hostAssigned = false;
};
