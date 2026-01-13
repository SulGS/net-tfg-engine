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
        std::lock_guard<std::mutex> lock(clientsMutex);
        clients.push_back(std::move(client));
        return clients.size() - 1;
    }

    // Activate a client (adds to active set)
    bool ActivateClient(size_t index) {
        std::lock_guard<std::mutex> lock(clientsMutex);
        if (index >= clients.size()) return false;

        auto it = std::find(activeClientIndices.begin(), activeClientIndices.end(), index);
        if (it == activeClientIndices.end()) {
            activeClientIndices.push_back(index);
        }
        return true;
    }

    // Deactivate a specific client
    bool DeactivateClient(size_t index) {
        std::lock_guard<std::mutex> lock(clientsMutex);
        auto it = std::find(activeClientIndices.begin(), activeClientIndices.end(), index);
        if (it != activeClientIndices.end()) {
            activeClientIndices.erase(it);
            return true;
        }
        return false;
    }

    // Deactivate all clients
    void DeactivateAll() {
        std::lock_guard<std::mutex> lock(clientsMutex);
        activeClientIndices.clear();
    }

    // Get all active clients
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

    // Get a specific client by index
    Client* GetClient(size_t index) {
        std::lock_guard<std::mutex> lock(clientsMutex);
        if (index >= clients.size()) return nullptr;
        return clients[index].get();
    }

    // Check if a specific client is active
    bool IsClientActive(size_t index) const {
        std::lock_guard<std::mutex> lock(clientsMutex);
        return std::find(activeClientIndices.begin(), activeClientIndices.end(), index)
            != activeClientIndices.end();
    }

    // Get all active client indices
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

    // ---- Legacy Single-Client Support ----
    // For backward compatibility with existing code
    bool SetActiveClient(size_t index) {
        std::lock_guard<std::mutex> lock(clientsMutex);
        if (index >= clients.size()) return false;

        // Clear all and set only this one as active
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

    // ---- Scene Switching Control ----
    void RequestClientSwitch(size_t newIndex) {
        if (newIndex < clients.size()) {
            pendingClientSwitch = true;
            pendingClientIndex = newIndex;
        }
    }

    // Apply pending switch (single-client mode)
    void ApplyPendingSwitch() {
        if (pendingClientSwitch) {
            SetActiveClient(pendingClientIndex);
            pendingClientSwitch = false;
        }
    }

    bool HasPendingSwitch() const { return pendingClientSwitch; }

    // ---- Running ----
    // Run all active clients (non-blocking update for each)
    void UpdateActiveClients() {
        auto activeClients = GetActiveClients();

        for (Client* client : activeClients) {
            if (client) {
                // Call client update method
                // client->Update();
            }
        }
    }

    // Run a specific client (for backward compatibility)
    int RunActiveClient(const std::string& hostStr = "0.0.0.0",
        uint16_t port = 0)
    {
        ApplyPendingSwitch();
        Client* client = GetActiveClient();
        if (!client) return -1;
        return client->SetupClient(hostStr, port);
    }

    // Run a specific client by index
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