#pragma once

#include <enet/enet.h>
#include "netcode/netcode_common.hpp"
#include "netcode/server_netcode.hpp"
#include "netcode/enet_session.hpp"
#include <set>
#include <map>
#include <vector>
#include <memory>
#include <chrono>
#include <iomanip>

// Configuration structure for server connection rules
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
    {}
};

// Packet types
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
    {}

    int RunServer() {

        if (enet_initialize() != 0) {
            std::cerr << "ENet initialization failed\n";
            return 1;
        }
        atexit(enet_deinitialize);

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
    // Member variables
    ENetSession net_;
    ServerRollbackNetcode rollback_;
    ServerConfig config_;
    std::map<ENetPeer*, PeerInfo> peerInfo_;
    std::vector<PeerInfo> allPlayers_;
    int currentFrame_;
    bool running_;
    size_t activePlayerCount_;
    std::set<int> pendingReconnections_;

    std::vector<long long> tickDurations_;
    const size_t MAX_SAMPLES = 30;

    // Helper methods
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

    void SendServerAccept(ENetPeer* peer, int playerId, bool isReconnection) {
        ServerAcceptPacket accept;
        accept.type = PACKET_SERVER_ACCEPT;
        accept.playerId = playerId;
        accept.isReconnection = isReconnection;
        
        ENetPacket* packet = enet_packet_create(&accept, 
            sizeof(accept), ENET_PACKET_FLAG_RELIABLE);
        enet_peer_send(peer, 0, packet);
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

    bool HandleNewConnectionInGame(ENetEvent& event, const std::string& clientId) {
        if (!config_.allowMidGameJoin) {
            std::cerr << "New connection rejected (mid-game join disabled): " << clientId << "\n";
            enet_peer_disconnect(event.peer, 0);
            return false;
        }

        if (peerInfo_.size() >= config_.maxPlayers) {
            std::cerr << "New connection rejected: server full (" 
                      << peerInfo_.size() << "/" << config_.maxPlayers << ")\n";
            enet_peer_disconnect(event.peer, 0);
            return false;
        }

        if (config_.requireClientId && !IsValidClientId(clientId)) {
            std::cerr << "New connection rejected (invalid client ID): " << clientId << "\n";
            enet_peer_disconnect(event.peer, 0);
            return false;
        }

        if (IsClientIdInUse(clientId)) {
            std::cerr << "New connection rejected (duplicate ID): " << clientId << "\n";
            enet_peer_disconnect(event.peer, 0);
            return false;
        }

        PeerInfo info;
        info.peer = event.peer;
        info.clientId = clientId;
        info.isConnected = true;
        info.playerId = static_cast<int>(allPlayers_.size());
        allPlayers_.push_back(info);
        peerInfo_[event.peer] = info;

        std::cerr << "Player " << info.playerId 
                  << " (" << clientId << ") joined mid-game\n";

        SendServerAccept(event.peer, info.playerId, false);
        activePlayerCount_++;

        return true;
    }

    bool HandleReconnection(ENetEvent& event, const std::string& clientId) {
        PeerInfo* playerInfo = FindPlayerByClientId(clientId);
        
        if (!playerInfo || playerInfo->isConnected) {
            std::cerr << "Unknown client attempted reconnection: " << clientId << "\n";
            enet_peer_disconnect(event.peer, 0);
            return false;
        }
        
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
            now - playerInfo->disconnectTime);
        
        if (elapsed > config_.reconnectionTimeout && config_.reconnectionTimeout.count() > 0) {
            std::cerr << "Reconnection timeout exceeded for " << clientId << "\n";
            enet_peer_disconnect(event.peer, 0);
            return false;
        }

        if (!playerInfo->isConnected)
            activePlayerCount_++;
        
        playerInfo->peer = event.peer;
        playerInfo->isConnected = true;
        peerInfo_[event.peer] = *playerInfo;
        
        std::cerr << "Player " << playerInfo->playerId << " (" << clientId 
                  << ") reconnected after " << elapsed.count() << "s\n";
        
        SendServerAccept(event.peer, playerInfo->playerId, true);
        
        return true;
    }

    void HandleInputPacket(ENetEvent& event) {
        uint8_t* data = event.packet->data;
        size_t len = event.packet->dataLength;
        // Expect: 1 (type) + 4 (playerId) + 4 (frame) + sizeof(InputBlob)
        const size_t EXPECTED_MIN_LEN = 1 + 4 + 4 + sizeof(InputBlob);
        if (len < EXPECTED_MIN_LEN) {
            std::cerr << "[SERVER] Malformed input packet, len=" << len << " (expected at least " << EXPECTED_MIN_LEN << ")\n";
            return;
        }

        InputEntry ie = net_.ParseInputEntryPacket(data, len);
        rollback_.OnClientInputReceived(ie.playerId, ie.frame, ie);

        for (auto& [peer, info] : peerInfo_) {
            if (!info.isConnected) {
                continue;
            } else {
                if (info.playerId != ie.playerId) {
                    net_.SendInputUpdate(peer, ie.playerId, ie.frame, ie.input);
                }
            }
        }
    }

    void HandleClientHelloDuringGame(ENetEvent& event) {
        if (!config_.allowReconnection) {
            enet_peer_disconnect(event.peer, 0);
            return;
        }
        
        uint8_t* data = event.packet->data;
        size_t len = event.packet->dataLength;
        
        if (len < sizeof(ClientHelloPacket)) {
            enet_peer_disconnect(event.peer, 0);
            return;
        }
        
        ClientHelloPacket* hello = (ClientHelloPacket*)data;
        std::string clientId(hello->clientId);

        if (!IsClientIdInUse(clientId)) {
            std::cerr << "Accepting new mid-game player with ID: " << clientId << "\n";
            HandleNewConnectionInGame(event, clientId);
        } else {
            std::cerr << "Client ID already in use, reconnecting: " << clientId << "\n";
            HandleReconnection(event, clientId);
        }
    }

    void HandleReceiveEventInGame(ENetEvent& event) {
        uint8_t* data = event.packet->data;
        size_t len = event.packet->dataLength;
        
        if (len < 1) {
            enet_packet_destroy(event.packet);
            return;
        }
        
        uint8_t type = data[0];
        
        if (type == PACKET_INPUT) {
            HandleInputPacket(event);
            enet_packet_destroy(event.packet);
            return;
        }
        
        if (type == PACKET_CLIENT_HELLO) {
            std::cout << "Received CLIENT_HELLO during game, len=" << len << "\n";
            HandleClientHelloDuringGame(event);
            enet_packet_destroy(event.packet);
            return;
        }
        
        std::cerr << "[SERVER] Received unknown or malformed packet of type " 
                  << (int)type << ", len=" << len << "\n";
        enet_packet_destroy(event.packet);
    }

    void HandleDisconnectInGame(ENetEvent& event) {
        auto it = peerInfo_.find(event.peer);
        
        if (it == peerInfo_.end()) {
            return;
        }
        
        std::cerr << "Player " << it->second.playerId << " disconnected\n";
        
        if (config_.allowReconnection) {
            it->second.isConnected = false;
            it->second.disconnectTime = std::chrono::steady_clock::now();
            
            for (auto& p : allPlayers_) {
                if (p.playerId == it->second.playerId) {
                    p.isConnected = false;
                    p.disconnectTime = it->second.disconnectTime;
                    break;
                }
            }
            
            if(config_.reconnectionTimeout.count() > 0)
            {
                std::cerr << "Player " << it->second.playerId 
                      << " can reconnect within " 
                      << config_.reconnectionTimeout.count() << " seconds\n";
            }
            else
            {
                std::cerr << "Player " << it->second.playerId 
                      << " can reconnect indefinitely\n";
            }
            
        }
        
        activePlayerCount_--;
        
        if (config_.stopOnBelowMin && activePlayerCount_ < config_.minPlayers) {
            std::cerr << "Not enough active clients (" << activePlayerCount_ 
                      << " < " << config_.minPlayers << "). Stopping server.\n";
            running_ = false;
        }
    }

    void HandleConnectInGame(ENetEvent& event) {
        std::cerr << "New connection during game, waiting for identification...\n";
    }

    size_t CountActivePlayers() {
        return std::count_if(peerInfo_.begin(), peerInfo_.end(),
            [](const auto& p) { return p.second.isConnected; });
    }

    void RunServerLoop() {
        auto nextTick = std::chrono::high_resolution_clock::now();
        activePlayerCount_ = CountActivePlayers();

        for (auto [peer, info] : peerInfo_) {
            rollback_.OnPlayerConnected(info.playerId);
        }

        while (running_ && (activePlayerCount_ >= config_.minPlayers || !config_.stopOnBelowMin)) {
            net_.Poll([&](ENetEvent &event) {
                if (event.type == ENET_EVENT_TYPE_RECEIVE) {
                    HandleReceiveEventInGame(event);
                    return;
                }
                
                if (event.type == ENET_EVENT_TYPE_DISCONNECT) {
                    HandleDisconnectInGame(event);
                    rollback_.OnPlayerDisconnected(event.peer->incomingPeerID);
                    return;
                }
                
                if (event.type == ENET_EVENT_TYPE_CONNECT) {
                    HandleConnectInGame(event);
                    pendingReconnections_.insert(event.peer->incomingPeerID);
                    enet_peer_timeout(event.peer, 100, 300, 500);
                    return;
                }
            });

            StateUpdate update = rollback_.Tick(currentFrame_);

            

            if (currentFrame_ % 30 == 0) {

                for (auto& [peer, info] : peerInfo_) {
                    if (!info.isConnected) {
                        continue;
                    } else {
                        if (pendingReconnections_.find(info.playerId) != pendingReconnections_.end()) {
                            rollback_.OnPlayerReconnected(info.playerId);
                            pendingReconnections_.erase(info.playerId);
                        } else {
                            net_.SendStateUpdate(peer, update);
                        }
                    }
                }





                auto now = std::chrono::high_resolution_clock::now();
                auto duration = std::chrono::duration_cast<std::chrono::microseconds>(now - nextTick);
                long long durationUs = duration.count();
                
                // Store the duration
                tickDurations_.push_back(durationUs);
                
                // Keep only the last MAX_SAMPLES
                if (tickDurations_.size() > MAX_SAMPLES) {
                    tickDurations_.erase(tickDurations_.begin());
                }
                
                // Calculate mean
                long long sum = 0;
                for (long long d : tickDurations_) {
                    sum += d;
                }
                double mean = static_cast<double>(sum) / tickDurations_.size();
                
                // Convert microseconds to milliseconds (divide by 1000, not 30)
                double currentMs = durationUs / 1000.0 / TICKS_PER_SECOND;
                double meanMs = mean / 1000.0 / TICKS_PER_SECOND;

                GameStateBlob s = rollback_.GetCurrentState();
                rollback_.GetGameLogic()->PrintState(s);
                
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

    bool HandleNewClient(ENetEvent& event, const std::string& clientId) {
        if (config_.requireClientId && !IsValidClientId(clientId)) {
            std::cerr << "Invalid client ID format: " << clientId << "\n";
            enet_peer_disconnect(event.peer, 0);
            return false;
        }
        
        PeerInfo* existingPlayer = FindPlayerByClientId(clientId);
        bool isReconnection = (existingPlayer != nullptr);
        
        if (isReconnection && config_.allowReconnection) {
            existingPlayer->peer = event.peer;
            existingPlayer->isConnected = true;
            peerInfo_[event.peer] = *existingPlayer;
            
            std::cerr << "Player " << existingPlayer->playerId 
                      << " (" << clientId << ") reconnected\n";
            
            SendServerAccept(event.peer, existingPlayer->playerId, true);
            return true;
        }
        
        if (!isReconnection && config_.requireClientId && !clientId.empty()) {
            if (IsClientIdInUse(clientId)) {
                std::cerr << "Duplicate client ID rejected: " << clientId << "\n";
                enet_peer_disconnect(event.peer, 0);
                return false;
            }
        }
        
        if (peerInfo_.size() >= config_.maxPlayers && !isReconnection) {
            std::cerr << "Connection rejected: server full\n";
            enet_peer_disconnect(event.peer, 0);
            return false;
        }
        
        PeerInfo info;
        info.peer = event.peer;
        info.clientId = clientId;
        info.isConnected = true;
        info.playerId = allPlayers_.size();
        
        allPlayers_.push_back(info);
        peerInfo_[event.peer] = info;
        
        std::cerr << "Client accepted as Player " << info.playerId 
                  << " (ID: " << clientId << ")"
                  << ". Total clients: " << peerInfo_.size() << "/" 
                  << config_.maxPlayers << "\n";
        
        SendServerAccept(event.peer, info.playerId, false);
        
        if (peerInfo_.size() == config_.minPlayers) {
            std::cerr << "Minimum players reached. Game will start.\n";
        }
        
        return true;
    }

    void HandleClientHello(ENetEvent& event) {
        uint8_t* data = event.packet->data;
        size_t len = event.packet->dataLength;
        
        if (len < sizeof(ClientHelloPacket)) {
            enet_peer_disconnect(event.peer, 0);
            return;
        }
        
        ClientHelloPacket* hello = (ClientHelloPacket*)data;
        std::string clientId(hello->clientId);
        
        HandleNewClient(event, clientId);
    }

    bool WaitForClients() {
        bool running = true;
        
        while (peerInfo_.size() < config_.minPlayers && running) {
            net_.Poll([&](ENetEvent &event) {
                if (event.type == ENET_EVENT_TYPE_CONNECT) {
                    std::cerr << "Client connecting, waiting for identification...\n";
                    enet_peer_timeout(event.peer, 100, 300, 500);
                    return;
                }
                
                if (event.type == ENET_EVENT_TYPE_RECEIVE) {
                    if (event.packet->dataLength < 1 || event.packet->data[0] != PACKET_CLIENT_HELLO) {
                        enet_packet_destroy(event.packet);
                        return;
                    }
                    
                    HandleClientHello(event);
                    enet_packet_destroy(event.packet);
                    return;
                }
                
                if (event.type == ENET_EVENT_TYPE_DISCONNECT) {
                    auto it = peerInfo_.find(event.peer);
                    if (it != peerInfo_.end()) {
                        peerInfo_.erase(it);
                    }
                    std::cerr << "Client disconnected. Total clients: " << peerInfo_.size() << "\n";
                    return;
                }
            });
            
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        
        return running && peerInfo_.size() >= config_.minPlayers;
    }

    void BroadcastGameStart() {
        for (const auto& [peer, info] : peerInfo_) {
            net_.BroadcastGameStart(peer, info.playerId);
        }
    }

    void PrintServerConfig() {
        std::cerr << "Waiting for " << config_.minPlayers << " clients to connect...\n";
        
        if (config_.requireClientId) {
            std::cerr << "Client ID validation is ENABLED\n";
        }
        
        if (config_.allowReconnection) {
            std::cerr << "Reconnection is ENABLED (timeout: ";

            if(config_.reconnectionTimeout.count()>0)
            {
                std::cerr << config_.reconnectionTimeout.count() << "s";
            }
            else
            {
                std::cerr << "infinity";
            }

            std::cerr << ")\n";
        }
    }
};