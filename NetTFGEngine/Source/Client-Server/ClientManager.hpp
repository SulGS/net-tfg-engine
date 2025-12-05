#pragma once
#include <memory>
#include <vector>
#include <string>
#include <cstdint>

#include "Client.hpp"

class ClientManager {
public:
    // ---- Singleton ----
    static ClientManager& Get() {
        static ClientManager instance;
        return instance;
    }

    // Prevent copy and move
    ClientManager(const ClientManager&) = delete;
    ClientManager& operator=(const ClientManager&) = delete;
    ClientManager(ClientManager&&) = delete;
    ClientManager& operator=(ClientManager&&) = delete;

    // ---- Client Management ----
    size_t AddClient(std::unique_ptr<Client> client) {
        clients.push_back(std::move(client));
        return clients.size() - 1;
    }

    bool SetActiveClient(size_t index) {
        if (index >= clients.size()) return false;
        activeClientIndex = index;
        return true;
    }

    Client* GetActiveClient() {
        if (activeClientIndex >= clients.size()) return nullptr;
        return clients[activeClientIndex].get();
    }

    size_t ClientCount() const { return clients.size(); }

    // ---- Scene Switching Control ----
    void RequestClientSwitch(size_t newIndex) {
        if (newIndex < clients.size()) {
            pendingClientSwitch = true;
            pendingClientIndex = newIndex;
        }
    }

    // Called by engine loop or before RunClient
    void ApplyPendingSwitch() {
        if (pendingClientSwitch) {
            SetActiveClient(pendingClientIndex);
            pendingClientSwitch = false;
        }
    }

    bool HasPendingSwitch() const { return pendingClientSwitch; }

    // ---- Running ----
    int RunActiveClient(const std::string& hostStr = "0.0.0.0",
        uint16_t port = 0)
    {
        ApplyPendingSwitch();

        Client* client = GetActiveClient();
        if (!client) return -1;

        return client->RunClient(hostStr, port);
    }

private:
    ClientManager() = default;

private:
    std::vector<std::unique_ptr<Client>> clients{};
    size_t activeClientIndex = static_cast<size_t>(-1);

    // Pending scene/client switching
    bool pendingClientSwitch = false;
    size_t pendingClientIndex = 0;
};
