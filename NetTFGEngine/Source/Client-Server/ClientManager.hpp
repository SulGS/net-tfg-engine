#pragma once
#include <memory>
#include <vector>
#include <string>
#include <cstdint>
#include <algorithm>
#include <mutex>
#include "Client.hpp"

class ClientManager {
public:
    static ClientManager& Get() {
        static ClientManager instance;
        return instance;
    }

    ClientManager(const ClientManager&) = delete;
    ClientManager& operator=(const ClientManager&) = delete;
    ClientManager(ClientManager&&) = delete;
    ClientManager& operator=(ClientManager&&) = delete;

    size_t AddClient(std::unique_ptr<Client> client) {
        std::lock_guard<std::mutex> lock(clientsMutex);
        clients.push_back(std::move(client));
        return clients.size() - 1;
    }

    bool ActivateClient(size_t index) {
        std::lock_guard<std::mutex> lock(clientsMutex);
        if (index >= clients.size()) return false;

        auto it = std::find(activeClientIndices.begin(), activeClientIndices.end(), index);
        if (it == activeClientIndices.end()) {
            activeClientIndices.push_back(index);
        }
        return true;
    }

    bool DeactivateClient(size_t index) {
        std::lock_guard<std::mutex> lock(clientsMutex);
        auto it = std::find(activeClientIndices.begin(), activeClientIndices.end(), index);
        if (it != activeClientIndices.end()) {
            activeClientIndices.erase(it);
            return true;
        }
        return false;
    }

    void DeactivateAll() {
        std::lock_guard<std::mutex> lock(clientsMutex);
        activeClientIndices.clear();
    }

    std::vector<Client*> GetActiveClients() {
        std::lock_guard<std::mutex> lock(clientsMutex);
        std::vector<Client*> active;
        for (size_t idx : activeClientIndices) {
            if (idx < clients.size()) {
                active.push_back(clients[idx].get());
            }
        }
        return active;
    }

    Client* GetClient(size_t index) {
        std::lock_guard<std::mutex> lock(clientsMutex);
        if (index >= clients.size()) return nullptr;
        return clients[index].get();
    }

    bool IsClientActive(size_t index) const {
        std::lock_guard<std::mutex> lock(clientsMutex);
        return std::find(activeClientIndices.begin(), activeClientIndices.end(), index)
            != activeClientIndices.end();
    }

    std::vector<size_t> GetActiveIndices() const {
        std::lock_guard<std::mutex> lock(clientsMutex);
        return activeClientIndices;
    }

    size_t ClientCount() const {
        std::lock_guard<std::mutex> lock(clientsMutex);
        return clients.size();
    }

    size_t ActiveClientCount() const {
        std::lock_guard<std::mutex> lock(clientsMutex);
        return activeClientIndices.size();
    }

    // Legacy single-client API, kept for backward compatibility
    bool SetActiveClient(size_t index) {
        std::lock_guard<std::mutex> lock(clientsMutex);
        if (index >= clients.size()) return false;

        activeClientIndices.clear();
        activeClientIndices.push_back(index);
        return true;
    }

    Client* GetActiveClient() {
        std::lock_guard<std::mutex> lock(clientsMutex);
        if (activeClientIndices.empty()) return nullptr;
        size_t index = activeClientIndices[0];
        if (index >= clients.size()) return nullptr;
        return clients[index].get();
    }

    void RequestClientSwitch(size_t newIndex) {
        if (newIndex < clients.size()) {
            pendingClientSwitch = true;
            pendingClientIndex = newIndex;
        }
    }

    void ApplyPendingSwitch() {
        if (pendingClientSwitch) {
            SetActiveClient(pendingClientIndex);
            pendingClientSwitch = false;
        }
    }

    bool HasPendingSwitch() const { return pendingClientSwitch; }

    void UpdateActiveClients() {
        auto activeClients = GetActiveClients();

        for (Client* client : activeClients) {
            if (client) {
            }
        }
    }

    int RunActiveClient(const std::string& hostStr = "0.0.0.0",
        uint16_t port = 0)
    {
        ApplyPendingSwitch();
        Client* client = GetActiveClient();
        if (!client) return -1;
        return client->SetupClient(hostStr, port);
    }

    int RunClient(size_t index, const std::string& hostStr = "0.0.0.0",
        uint16_t port = 0)
    {
        Client* client = GetClient(index);
        if (!client) return -1;
        return client->SetupClient(hostStr, port);
    }

private:
    ClientManager() = default;

private:
    std::vector<std::unique_ptr<Client>> clients{};
    std::vector<size_t> activeClientIndices{};  // Support multiple active clients
    mutable std::mutex clientsMutex;  // Thread safety for client operations

    // Pending scene/client switching (for single-client legacy mode)
    bool pendingClientSwitch = false;
    size_t pendingClientIndex = 0;
};