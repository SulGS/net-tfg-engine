#pragma once

#include <GameNetworkingSockets/steam/steamnetworkingtypes.h>
#include <GameNetworkingSockets/steam/steamnetworkingsockets.h>
#include "netcode/netcode_common.hpp"
#include "netcode/server_netcode.hpp"
#include "netcode/valve_sockets_session.hpp"
#include <set>
#include <map>
#include <vector>
#include <memory>
#include <chrono>
#include <iomanip>

struct ServerConfig {
    uint16_t port;
    size_t minPlayers;
    size_t maxPlayers;
    bool allowMidGameJoin;
    bool stopOnBelowMin;
    bool allowReconnection;
    bool requireClientId;
    int maxFrames;
    std::chrono::seconds reconnectionTimeout;

    ServerConfig(uint16_t p = 7777)
        : port(p)
        , minPlayers(2)
        , maxPlayers(2)
        , allowMidGameJoin(false)
        , stopOnBelowMin(true)
        , allowReconnection(false)
        , requireClientId(false)
        , maxFrames(0)
        , reconnectionTimeout(30)
    {
    }
};

constexpr uint8_t PACKET_CLIENT_HELLO = 10;
constexpr uint8_t PACKET_SERVER_ACCEPT = 11;
constexpr uint8_t PACKET_SERVER_REJECT = 12;

struct ClientHelloPacket {
    uint8_t type;
    char clientId[64];
};

struct ServerAcceptPacket {
    uint8_t type;
    int playerId;
    bool isReconnection;
};

class RollbackServer {
public:
    RollbackServer(std::unique_ptr<IGameLogic> gameLogic,
        const ServerConfig& config = ServerConfig())
        : rollback_(std::move(gameLogic))
        , config_(config)
        , currentFrame_(0)
        , running_(true)
        , activePlayerCount_(0)
    {
    }

    int RunServer() {
        if (!net_.InitGNS()) {
            return 1;
        }

        if (!net_.InitHost(config_.port)) {
            return 1;
        }

        PrintServerConfig();

        if (!WaitForClients()) {
            return 1;
        }

        BroadcastGameStart();

        std::cerr << "Starting server game loop.\n";
        RunServerLoop();

        return 0;
    }

private:
    GNSSession net_;
    ServerRollbackNetcode rollback_;
    ServerConfig config_;
    std::map<HSteamNetConnection, PeerInfo> peerInfo_;
    std::vector<PeerInfo> allPlayers_;
    int currentFrame_;
    bool running_;
    size_t activePlayerCount_;
    std::set<int> pendingReconnections_;

    std::vector<long long> tickDurations_;
    const size_t MAX_SAMPLES = 30;

    bool IsValidClientId(const std::string& clientId) {
        if (clientId.empty() || clientId.length() > 63) {
            return false;
        }

        for (char c : clientId) {
            if (!std::isalnum(c) && c != '-' && c != '_') {
                return false;
            }
        }
        return true;
    }

    void SendServerAccept(HSteamNetConnection conn, int playerId, bool isReconnection) {
        ServerAcceptPacket accept;
        accept.type = PACKET_SERVER_ACCEPT;
        accept.playerId = playerId;
        accept.isReconnection = isReconnection;

        ISteamNetworkingSockets* sockets = net_.GetSockets();
        if (sockets) {
            sockets->SendMessageToConnection(conn, &accept,
                sizeof(accept), k_nSteamNetworkingSend_Reliable, nullptr);
        }
    }

    PeerInfo* FindPlayerByClientId(const std::string& clientId) {
        auto it = std::find_if(allPlayers_.begin(), allPlayers_.end(),
            [&clientId](const PeerInfo& p) { return p.clientId == clientId; });

        if (it == allPlayers_.end()) {
            return nullptr;
        }

        return &(*it);
    }

    bool IsClientIdInUse(const std::string& clientId) {
        auto it = std::find_if(peerInfo_.begin(), peerInfo_.end(),
            [&clientId](const auto& p) {
                return p.second.clientId == clientId;
            });

        return it != peerInfo_.end();
    }

    bool HandleNewConnectionInGame(HSteamNetConnection conn, const std::string& clientId) {
        if (!config_.allowMidGameJoin) {
            std::cerr << "New connection rejected (mid-game join disabled): " << clientId << "\n";
            net_.GetSockets()->CloseConnection(conn, k_ESteamNetConnectionEnd_App_Generic, nullptr, false);
            return false;
        }

        if (peerInfo_.size() >= config_.maxPlayers) {
            std::cerr << "New connection rejected: server full ("
                << peerInfo_.size() << "/" << config_.maxPlayers << ")\n";
            net_.GetSockets()->CloseConnection(conn, k_ESteamNetConnectionEnd_App_Generic, nullptr, false);
            return false;
        }

        if (config_.requireClientId && !IsValidClientId(clientId)) {
            std::cerr << "New connection rejected (invalid client ID): " << clientId << "\n";
            net_.GetSockets()->CloseConnection(conn, k_ESteamNetConnectionEnd_App_Generic, nullptr, false);
            return false;
        }

        if (IsClientIdInUse(clientId)) {
            std::cerr << "New connection rejected (duplicate ID): " << clientId << "\n";
            net_.GetSockets()->CloseConnection(conn, k_ESteamNetConnectionEnd_App_Generic, nullptr, false);
            return false;
        }

        PeerInfo info;
        info.connection = conn;
        info.clientId = clientId;
        info.isConnected = true;
        info.playerId = static_cast<int>(allPlayers_.size());
        allPlayers_.push_back(info);
        peerInfo_[conn] = info;

        std::cerr << "Player " << info.playerId
            << " (" << clientId << ") joined mid-game\n";

        SendServerAccept(conn, info.playerId, false);
        activePlayerCount_++;

        return true;
    }

    bool HandleReconnection(HSteamNetConnection conn, const std::string& clientId) {
        PeerInfo* playerInfo = FindPlayerByClientId(clientId);

        if (!playerInfo || playerInfo->isConnected) {
            std::cerr << "Unknown client attempted reconnection: " << clientId << "\n";
            net_.GetSockets()->CloseConnection(conn, k_ESteamNetConnectionEnd_App_Generic, nullptr, false);
            return false;
        }

        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
            now - playerInfo->disconnectTime);

        if (elapsed > config_.reconnectionTimeout && config_.reconnectionTimeout.count() > 0) {
            std::cerr << "Reconnection timeout exceeded for " << clientId << "\n";
            net_.GetSockets()->CloseConnection(conn, k_ESteamNetConnectionEnd_App_Generic, nullptr, false);
            return false;
        }

        if (!playerInfo->isConnected)
            activePlayerCount_++;

        playerInfo->connection = conn;
        playerInfo->isConnected = true;
        peerInfo_[conn] = *playerInfo;

        std::cerr << "Player " << playerInfo->playerId << " (" << clientId
            << ") reconnected after " << elapsed.count() << "s\n";

        SendServerAccept(conn, playerInfo->playerId, true);

        return true;
    }

    void HandleInputPacket(HSteamNetConnection conn, const uint8_t* data, int len) {
        const size_t EXPECTED_MIN_LEN = 1 + 4 + 4 + sizeof(InputBlob);
        if (len < EXPECTED_MIN_LEN) {
            std::cerr << "[SERVER] Malformed input packet, len=" << len << "\n";
            return;
        }

        InputEntry ie = net_.ParseInputEntryPacket(data, len);

        if (ie.frame < currentFrame_)
        {
            ie.frame = currentFrame_;
        }

        rollback_.OnClientInputReceived(ie.playerId, ie.frame, ie);

        for (auto& [peer, info] : peerInfo_) {
            if (!info.isConnected) {
                continue;
            }
            if (info.playerId != ie.playerId) {
                net_.SendInputUpdate(peer, ie.playerId, ie.frame, ie.input);
            }
        }
    }

    void HandleClientHelloDuringGame(HSteamNetConnection conn, const uint8_t* data, int len) {
        if (!config_.allowReconnection) {
            net_.GetSockets()->CloseConnection(conn, k_ESteamNetConnectionEnd_App_Generic, nullptr, false);
            return;
        }

        if (len < sizeof(ClientHelloPacket)) {
            net_.GetSockets()->CloseConnection(conn, k_ESteamNetConnectionEnd_App_Generic, nullptr, false);
            return;
        }

        const ClientHelloPacket* hello = (const ClientHelloPacket*)data;
        std::string clientId(hello->clientId);

        std::cerr << "Client attempting connection/reconnection during game: " << clientId << "\n";

        // Add to poll group
        net_.AddConnectionToPollGroup(conn);

        // Use the same handler for consistency
        HandleNewClient(conn, clientId);
    }

    void HandleReceiveEventInGame(HSteamNetConnection conn, const uint8_t* data, int len) {
        if (len < 1) {
            return;
        }

        uint8_t type = data[0];

        if (type == PACKET_INPUT) {
            HandleInputPacket(conn, data, len);
            return;
        }

        if (type == PACKET_CLIENT_HELLO) {
            std::cout << "Received CLIENT_HELLO during game, len=" << len << "\n";
            HandleClientHelloDuringGame(conn, data, len);
            return;
        }

        if (type == PACKET_INPUT_DELAY) {
            InputDelayPacket packet = net_.ParseInputDelaySync(data, len);

            packet.recframe = currentFrame_;

            net_.SendInputDelaySync(conn, packet);


            return;
        }

        std::cerr << "[SERVER] Received unknown packet type " << (int)type << ", len=" << len << "\n";
    }

    void HandleDisconnectInGame(HSteamNetConnection conn) {
        auto it = peerInfo_.find(conn);

        if (it == peerInfo_.end()) {
            return;
        }

        int playerId = it->second.playerId;
        std::cerr << "Player " << playerId << " disconnected\n";

        if (config_.allowReconnection) {
            // Mark as disconnected but keep in allPlayers_ for reconnection
            it->second.isConnected = false;
            it->second.disconnectTime = std::chrono::steady_clock::now();

            for (auto& p : allPlayers_) {
                if (p.playerId == playerId) {
                    p.isConnected = false;
                    p.disconnectTime = it->second.disconnectTime;
                    break;
                }
            }

            if (config_.reconnectionTimeout.count() > 0) {
                std::cerr << "Player " << playerId
                    << " can reconnect within "
                    << config_.reconnectionTimeout.count() << " seconds\n";
            }
            else {
                std::cerr << "Player " << playerId
                    << " can reconnect indefinitely\n";
            }
        }

        peerInfo_.erase(it);
        activePlayerCount_--;

        if (config_.stopOnBelowMin && activePlayerCount_ < config_.minPlayers) {
            std::cerr << "Not enough active clients (" << activePlayerCount_
                << " < " << config_.minPlayers << "). Stopping server.\n";
            running_ = false;
        }
    }

    void HandleConnectInGame(HSteamNetConnection conn) {
        std::cerr << "New connection during game, waiting for identification...\n";
    }

    size_t CountActivePlayers() {
        return std::count_if(peerInfo_.begin(), peerInfo_.end(),
            [](const auto& p) { return p.second.isConnected; });
    }

    void PollConnectionEvents() {
        ISteamNetworkingSockets* sockets = net_.GetSockets();
        if (!sockets) return;

        SteamNetConnectionStatusChangedCallback_t callback;
        while (sockets->GetConnectionUserData(net_.GetListenSocket())) {
            // This is a simplified approach; in production you'd use proper callbacks
            // For now we rely on message polling to detect connection state changes
            break;
        }
    }

    void RunServerLoop() {
        auto nextTick = std::chrono::high_resolution_clock::now();
        activePlayerCount_ = CountActivePlayers();

        for (auto [conn, info] : peerInfo_) {
            rollback_.OnPlayerConnected(info.playerId);
        }

        while (running_ && (activePlayerCount_ >= config_.minPlayers || !config_.stopOnBelowMin)) {
            net_.PumpCallbacks();  // CRITICAL: Pump callbacks to process connection state changes
            net_.Poll([&](const uint8_t* data, int len, HSteamNetConnection conn) {
                HandleReceiveEventInGame(conn, data, len);
                });

            // Check for disconnections (simplified polling)
            ISteamNetworkingSockets* sockets = net_.GetSockets();
            std::vector<HSteamNetConnection> toRemove;

            for (auto& [conn, info] : peerInfo_) {
                SteamNetConnectionInfo_t connInfo;
                if (sockets->GetConnectionInfo(conn, &connInfo)) {
                    if (connInfo.m_eState == k_ESteamNetworkingConnectionState_ClosedByPeer ||
                        connInfo.m_eState == k_ESteamNetworkingConnectionState_ProblemDetectedLocally ||
                        connInfo.m_eState == k_ESteamNetworkingConnectionState_Dead) {
                        toRemove.push_back(conn);
                    }
                }
            }

            for (auto conn : toRemove) {
                HandleDisconnectInGame(conn);
                rollback_.OnPlayerDisconnected(peerInfo_[conn].playerId);
            }

            StateUpdate update = rollback_.Tick(currentFrame_);

            if (currentFrame_ % 30 == 0) {
                for (auto& [conn, info] : peerInfo_) {
                    if (!info.isConnected) {
                        continue;
                    }
                    if (pendingReconnections_.find(info.playerId) != pendingReconnections_.end()) {
                        rollback_.OnPlayerReconnected(info.playerId);
                        pendingReconnections_.erase(info.playerId);
                    }
                    else {
                        net_.SendStateUpdate(conn, update);
                    }
                }

                auto now = std::chrono::high_resolution_clock::now();
                auto duration = std::chrono::duration_cast<std::chrono::microseconds>(now - nextTick);
                long long durationUs = duration.count();

                tickDurations_.push_back(durationUs);

                if (tickDurations_.size() > MAX_SAMPLES) {
                    tickDurations_.erase(tickDurations_.begin());
                }

                long long sum = 0;
                for (long long d : tickDurations_) {
                    sum += d;
                }
                double mean = static_cast<double>(sum) / tickDurations_.size();

                double currentMs = durationUs / 1000.0 / TICKS_PER_SECOND;
                double meanMs = mean / 1000.0 / TICKS_PER_SECOND;

                GameStateBlob s = rollback_.GetCurrentState();
                //rollback_.GetGameLogic()->PrintState(s);

                std::cout << "Current: " << std::fixed << std::setprecision(5) << currentMs << " ms | "
                    << "Mean (last " << tickDurations_.size() << "): "
                    << meanMs << " ms" << std::endl;
            }

            ++currentFrame_;
            nextTick += std::chrono::milliseconds(MS_PER_TICK);
            std::this_thread::sleep_until(nextTick);

            if (config_.maxFrames > 0 && currentFrame_ > config_.maxFrames) {
                std::cerr << "Reached maximum frames. Stopping server.\n";
                running_ = false;
            }
        }
    }

    bool HandleNewClient(HSteamNetConnection conn, const std::string& clientId) {
        if (config_.requireClientId && !IsValidClientId(clientId)) {
            std::cerr << "Invalid client ID format: " << clientId << "\n";
            net_.GetSockets()->CloseConnection(conn, k_ESteamNetConnectionEnd_App_Generic, nullptr, false);
            return false;
        }

        PeerInfo* existingPlayer = FindPlayerByClientId(clientId);

        // Check if this is a reconnection (player exists but is not currently connected)
        bool isReconnection = (existingPlayer != nullptr && !existingPlayer->isConnected);

        // Check if player is already connected (duplicate connection attempt)
        bool isAlreadyConnected = (existingPlayer != nullptr && existingPlayer->isConnected);

        if (isAlreadyConnected) {
            std::cerr << "Duplicate client ID already connected: " << clientId << "\n";
            std::cerr << "DEBUG: Existing connection is " << existingPlayer->connection << ", new is " << conn << "\n";

            // Force close the old connection and accept the new one as reconnection
            auto oldConnIt = peerInfo_.find(existingPlayer->connection);
            if (oldConnIt != peerInfo_.end()) {
                std::cerr << "Forcing old connection closed and treating as reconnection\n";
                peerInfo_.erase(oldConnIt);
                net_.GetSockets()->CloseConnection(existingPlayer->connection, 0, nullptr, false);
                isReconnection = true;
            }
        }

        if (isReconnection && config_.allowReconnection) {
            auto now = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                now - existingPlayer->disconnectTime);

            // Check reconnection timeout
            if (elapsed > config_.reconnectionTimeout && config_.reconnectionTimeout.count() > 0) {
                std::cerr << "Reconnection timeout exceeded for " << clientId << "\n";
                net_.GetSockets()->CloseConnection(conn, k_ESteamNetConnectionEnd_App_Generic, nullptr, false);
                return false;
            }

            // Update with new connection
            existingPlayer->connection = conn;
            existingPlayer->isConnected = true;
            peerInfo_[conn] = *existingPlayer;
            activePlayerCount_++;

            std::cerr << "Player " << existingPlayer->playerId
                << " (" << clientId << ") reconnected after "
                << elapsed.count() << "s\n";

            SendServerAccept(conn, existingPlayer->playerId, true);

            StateUpdate currentUpdate = rollback_.Tick(currentFrame_);
            net_.SendStateUpdate(conn, currentUpdate);
            std::cerr << "Sent state update to reconnected player " << existingPlayer->playerId << "\n";

            return true;
        }

        // Not a reconnection - new player
        if (peerInfo_.size() >= config_.maxPlayers) {
            std::cerr << "Connection rejected: server full\n";
            net_.GetSockets()->CloseConnection(conn, k_ESteamNetConnectionEnd_App_Generic, nullptr, false);
            return false;
        }

        PeerInfo info;
        info.connection = conn;
        info.clientId = clientId;
        info.isConnected = true;
        info.playerId = allPlayers_.size();

        allPlayers_.push_back(info);
        peerInfo_[conn] = info;
        activePlayerCount_++;

        std::cerr << "Client accepted as Player " << info.playerId
            << " (ID: " << clientId << ")"
            << ". Total clients: " << peerInfo_.size() << "/"
            << config_.maxPlayers << "\n";

        SendServerAccept(conn, info.playerId, false);

        if (peerInfo_.size() == config_.minPlayers) {
            std::cerr << "Minimum players reached. Game will start.\n";
        }

        return true;
    }

    void HandleClientHello(HSteamNetConnection conn, const uint8_t* data, int len) {
        if (len < sizeof(ClientHelloPacket)) {
            net_.GetSockets()->CloseConnection(conn, k_ESteamNetConnectionEnd_App_Generic, nullptr, false);
            return;
        }

        const ClientHelloPacket* hello = (const ClientHelloPacket*)data;
        std::string clientId(hello->clientId);

        std::cerr << "Received CLIENT_HELLO from " << clientId << "\n";

        // Add to poll group BEFORE handling (in case it's already connected via callback)
        net_.AddConnectionToPollGroup(conn);

        HandleNewClient(conn, clientId);
    }

    bool WaitForClients() {
        bool running = true;

        while (peerInfo_.size() < config_.minPlayers && running) {
            net_.PumpCallbacks();  // CRITICAL: Pump callbacks to process connection state changes
            net_.Poll([&](const uint8_t* data, int len, HSteamNetConnection conn) {
                if (len < 1) {
                    return;
                }

                if (data[0] != PACKET_CLIENT_HELLO) {
                    return;
                }

                HandleClientHello(conn, data, len);
                });

            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        return running && peerInfo_.size() >= config_.minPlayers;
    }

    void BroadcastGameStart() {
        for (const auto& [conn, info] : peerInfo_) {
            net_.BroadcastGameStart(conn, info.playerId);
        }
    }

    void PrintServerConfig() {
        std::cerr << "Waiting for " << config_.minPlayers << " clients to connect...\n";

        if (config_.requireClientId) {
            std::cerr << "Client ID validation is ENABLED\n";
        }

        if (config_.allowReconnection) {
            std::cerr << "Reconnection is ENABLED (timeout: ";

            if (config_.reconnectionTimeout.count() > 0) {
                std::cerr << config_.reconnectionTimeout.count() << "s";
            }
            else {
                std::cerr << "infinity";
            }

            std::cerr << ")\n";
        }
    }
};