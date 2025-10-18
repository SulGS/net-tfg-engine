#pragma once

#include <enet/enet.h>
#include "netcode/netcode_common.hpp"
#include "netcode/client_netcode.hpp"
#include "netcode/client_window.hpp"
#include "netcode/enet_session.hpp"
#include "OpenGL/IGameRenderer.hpp"
#include <memory>
#include <string>
#include <chrono>
#include <thread>

class RollbackClient {
public:
    RollbackClient(std::unique_ptr<IGameLogic> gameLogic,
                   std::unique_ptr<IGameRenderer> gameRenderer,
                   const std::string& customClientId = "")
        : gameLogic_(std::move(gameLogic))
        , gameRenderer_(std::move(gameRenderer))
        , clientId_(customClientId.empty() ? GenerateClientId() : customClientId)
        , assignedPlayerId_(-1)
        , isReconnection_(false)
        , serverPeer_(nullptr)
    {}

    int RunClient(const std::string& hostStr, uint16_t port) {

        if (enet_initialize() != 0) {
            std::cerr << "ENet initialization failed\n";
            return 1;
        }
        atexit(enet_deinitialize);



        if (!net_.ConnectTo(hostStr, port)) {
            return 1;
        }
        
        std::cerr << "Using client ID: " << clientId_ << "\n";
        std::cerr << "Waiting for connection to server...\n";
        
        // Phase 1: Connect and authenticate
        if (!WaitForConnectionAndAuth()) {
            return 1;
        }
        
        if (isReconnection_) {
            std::cerr << "Successfully reconnected!\n";
        } else {
            std::cerr << "Successfully authenticated!\n";
            
            // Phase 2: Wait for game start
            if (!WaitForGameStart()) {
                return 1;
            }
        }
        
        std::cerr << "Before creating prediction netcode\n";

        gameRenderer_->playerId = assignedPlayerId_;
        
        ClientPredictionNetcode prediction(assignedPlayerId_, std::move(gameLogic_));
        ClientWindow cWindow(
            [this](GameStateBlob& state, OpenGLWindow* win) { 
                gameRenderer_->Init(state, win); 
            },
            [this](GameStateBlob& state, OpenGLWindow* win) { 
                gameRenderer_->Render(state, win); 
            }
        );

        prediction.GetGameLogic()->playerId = assignedPlayerId_;
        
        std::cerr << "Before running client loop\n";
        RunClientLoop(prediction, cWindow);
        
        return 0;
    }

    const std::string& GetClientId() const { return clientId_; }

private:
    ENetSession net_;
    std::unique_ptr<IGameLogic> gameLogic_;
    std::unique_ptr<IGameRenderer> gameRenderer_;
    std::string clientId_;
    int assignedPlayerId_;
    bool isReconnection_;
    ENetPeer* serverPeer_;

    std::string GenerateClientId() {
        auto now = std::chrono::system_clock::now();
        auto timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
            now.time_since_epoch()).count();
        
        return "client_" + std::to_string(timestamp);
    }

    bool SendClientHello() {
        ClientHelloPacket hello;
        hello.type = PACKET_CLIENT_HELLO;
        
        std::strncpy(hello.clientId, clientId_.c_str(), sizeof(hello.clientId) - 1);
        hello.clientId[sizeof(hello.clientId) - 1] = '\0';
        
        ENetPacket* packet = enet_packet_create(&hello, 
            sizeof(hello), ENET_PACKET_FLAG_RELIABLE);
        
        if (!packet) {
            std::cerr << "Failed to create client hello packet\n";
            return false;
        }
        
        enet_peer_send(serverPeer_, 0, packet);
        enet_host_flush(serverPeer_->host);
        
        std::cerr << "Sent CLIENT_HELLO with ID: " << clientId_ << "\n";
        return true;
    }

    bool HandleServerAccept(ENetEvent& event) {
        uint8_t* data = event.packet->data;
        size_t len = event.packet->dataLength;
        
        if (len < sizeof(ServerAcceptPacket)) {
            std::cerr << "Malformed SERVER_ACCEPT packet\n";
            return false;
        }
        
        ServerAcceptPacket* accept = (ServerAcceptPacket*)data;
        assignedPlayerId_ = accept->playerId;
        isReconnection_ = accept->isReconnection;
        
        if (isReconnection_) {
            std::cerr << "Reconnected as Player ID: " << assignedPlayerId_ << "\n";
        } else {
            std::cerr << "Assigned Player ID: " << assignedPlayerId_ << "\n";
        }
        
        return true;
    }

    bool HandleServerReject(ENetEvent& event) {
        std::cerr << "Server rejected connection\n";
        return false;
    }

    bool HandleGameStart(ENetEvent& event, bool& gameStarted) {
        uint8_t* data = event.packet->data;
        size_t len = event.packet->dataLength;
        
        if (len < 1 + 4) {
            std::cerr << "Malformed GAME_START packet\n";
            return false;
        }
        
        if (data[0] != PACKET_GAME_START) {
            return false;
        }
        
        uint32_t playerIdNet = 0;
        std::memcpy(&playerIdNet, data + 1, 4);
        int playerIdFromStart = bigEndianToHost32(playerIdNet);
        
        if (assignedPlayerId_ != -1 && assignedPlayerId_ != playerIdFromStart) {
            std::cerr << "WARNING: GAME_START player ID mismatch ("
                      << playerIdFromStart << " vs " << assignedPlayerId_ << ")\n";
        }
        
        assignedPlayerId_ = playerIdFromStart;
        gameStarted = true;
        
        std::cerr << "Game starting with Player ID: " << assignedPlayerId_ << "\n";
        return true;
    }

    bool HandleReceiveEvent(ENetEvent& event, bool& gameStarted, bool& serverAccepted) {
        uint8_t* data = event.packet->data;
        size_t len = event.packet->dataLength;
        
        if (len < 1) {
            enet_packet_destroy(event.packet);
            return true;
        }
        
        uint8_t type = data[0];
        
        if (type == PACKET_SERVER_ACCEPT) {
            if (!HandleServerAccept(event)) {
                enet_packet_destroy(event.packet);
                return false;
            }
            serverAccepted = true;
            enet_packet_destroy(event.packet);
            return true;
        }
        
        if (type == PACKET_SERVER_REJECT) {
            enet_packet_destroy(event.packet);
            return HandleServerReject(event);
        }
        
        if (type == PACKET_GAME_START) {
            if (!HandleGameStart(event, gameStarted)) {
                enet_packet_destroy(event.packet);
                return false;
            }
            enet_packet_destroy(event.packet);
            return true;
        }
        
        std::cerr << "Received unknown packet type: " << (int)type << "\n";
        enet_packet_destroy(event.packet);
        return true;
    }

    bool WaitForConnectionAndAuth() {
        bool connected = false;
        bool serverAccepted = false;
        bool running = true;
        
        while (!connected && running) {
            net_.Poll([&](ENetEvent &event) {
                if (event.type == ENET_EVENT_TYPE_CONNECT) {
                    connected = true;
                    serverPeer_ = event.peer;
                    std::cerr << "Connected to server\n";
                    
                    if (!SendClientHello()) {
                        running = false;
                    }
                    return;
                }
                
                if (event.type == ENET_EVENT_TYPE_DISCONNECT) {
                    std::cerr << "Connection to server failed\n";
                    running = false;
                    return;
                }
            });
            
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        
        if (!running || !connected) {
            return false;
        }
        
        std::cerr << "Waiting for server acceptance...\n";
        auto timeout = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        
        while (!serverAccepted && running) {
            if (std::chrono::steady_clock::now() > timeout) {
                std::cerr << "Timeout waiting for server acceptance\n";
                return false;
            }
            
            net_.Poll([&](ENetEvent &event) {
                if (event.type == ENET_EVENT_TYPE_RECEIVE) {
                    bool gameStarted = false;
                    if (!HandleReceiveEvent(event, gameStarted, serverAccepted)) {
                        running = false;
                    }
                    return;
                }
                
                if (event.type == ENET_EVENT_TYPE_DISCONNECT) {
                    std::cerr << "Disconnected during authentication\n";
                    running = false;
                    return;
                }
            });
            
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        
        return running && serverAccepted && assignedPlayerId_ != -1;
    }

    bool WaitForGameStart() {
        bool gameStarted = false;
        bool running = true;
        
        std::cerr << "Waiting for game to start...\n";
        auto timeout = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        
        while (!gameStarted && running) {
            if (std::chrono::steady_clock::now() > timeout) {
                std::cerr << "Timeout waiting for game start\n";
                return false;
            }
            
            net_.Poll([&](ENetEvent &event) {
                if (event.type == ENET_EVENT_TYPE_RECEIVE) {
                    bool serverAccepted = false;
                    if (!HandleReceiveEvent(event, gameStarted, serverAccepted)) {
                        running = false;
                    }
                    return;
                }
                
                if (event.type == ENET_EVENT_TYPE_DISCONNECT) {
                    std::cerr << "Disconnected before game start\n";
                    running = false;
                    return;
                }
            });
            
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        
        return running && gameStarted;
    }

    void RunClientLoop(ClientPredictionNetcode& prediction, ClientWindow& cWindow) {
        int currentFrame = 0;
        auto nextTick = std::chrono::high_resolution_clock::now();

        std::thread renderThread(&ClientWindow::run, &cWindow);

        while (cWindow.isRunning()) {
            net_.Poll([&](ENetEvent &event) {
                if (event.type == ENET_EVENT_TYPE_RECEIVE) {
                    uint8_t* data = event.packet->data;
                    size_t len = event.packet->dataLength;
                    uint8_t type = data[0];

                    if (type == PACKET_STATE_UPDATE) {
                        
                        bool correctPacket = true;
                        uint32_t lenVector = 0;

                        uint32_t lenState = 0;
                        std::memcpy(&lenState, data + 1 + 4, sizeof(uint32_t));
                        lenState = bigEndianToHost32(lenState);

                        if (len >= 1 + 4 + 4 + lenState + 4) {
                            uint32_t countRaw = 0;
                            std::memcpy(&countRaw, data + 1 + 4 + 4 + lenState, sizeof(uint32_t));
                            lenVector = bigEndianToHost32(countRaw);

                            size_t expectedLen = 1 + 4 + 4 + lenState + 4 + lenVector * (4 + sizeof(InputBlob));

                            if (len != expectedLen) {
                                correctPacket = false;
                            }
                        } else {
                            correctPacket = false;
                        }

                        if (correctPacket) {
                            StateUpdate update = net_.ParseStateUpdate(data, len);

                            if (currentFrame < update.frame) {
                                currentFrame = update.frame;
                            }
                            prediction.OnServerStateUpdate(update);
                        } else {
                            std::cerr << "[CLIENT] Received unknown or malformed packet of type "
                                    << (int)type 
                                    << ", len=" << len 
                                    << " (expected=" 
                                    << (1 + 4 + 4 + lenState + 4 + lenVector * (4 + sizeof(InputBlob))) 
                                    << ")\n";
                        }
                    }
                    else if(type == PACKET_INPUT_UPDATE)
                    {
                        const size_t EXPECTED_MIN_LEN = 1 + 4 + 4 + sizeof(InputBlob);

                        if (len < EXPECTED_MIN_LEN) {
                            std::cerr << "[SERVER] Malformed input packet, len=" << len << " (expected at least " << EXPECTED_MIN_LEN << ")\n";
                        }
                        else
                        {
                            InputEntry ie = net_.ParseInputEntryPacket(data, len);
                            prediction.OnServerInputUpdate(ie);
                        }

                        
                    }

                    enet_packet_destroy(event.packet);
                } else if (event.type == ENET_EVENT_TYPE_DISCONNECT) {
                    std::cerr << "Disconnected from server\n";
                    cWindow.close();
                }
            });

            InputBlob localInput = prediction.GetGameLogic()->GenerateLocalInput();
            prediction.SubmitLocalInput(currentFrame + INPUT_DELAY_FRAMES, localInput);
            net_.SendInput(serverPeer_, assignedPlayerId_, currentFrame + INPUT_DELAY_FRAMES, localInput);

            prediction.PredictToFrame(currentFrame);
            cWindow.setRenderState(prediction.GetCurrentState());

            GameStateBlob s = prediction.GetCurrentState();
            if (currentFrame % 30 == 0) {
                prediction.GetGameLogic()->PrintState(s);
            }
            ++currentFrame;

            nextTick += std::chrono::milliseconds(MS_PER_TICK);
            std::this_thread::sleep_until(nextTick);

            if (currentFrame > 200000) cWindow.close();
        }


        renderThread.join();

        if (serverPeer_ && serverPeer_->state == ENET_PEER_STATE_CONNECTED) {
            std::cout << "[CLIENT] Window closed, sending disconnect..." << std::endl;
            enet_peer_disconnect(serverPeer_, 0);

            // Give ENet a chance to flush the disconnect
            ENetEvent event;
            while (enet_host_service(net_.GetHost(), &event, 3000) > 0) {
                if (event.type == ENET_EVENT_TYPE_RECEIVE) {
                    enet_packet_destroy(event.packet);
                }
                else if (event.type == ENET_EVENT_TYPE_DISCONNECT) {
                    std::cout << "[CLIENT] Disconnected cleanly from server." << std::endl;
                    break;
                }
            }
        }
    }
};