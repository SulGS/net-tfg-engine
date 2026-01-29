#pragma once

#include <GameNetworkingSockets/steam/steamnetworkingtypes.h>
#include <GameNetworkingSockets/steam/steamnetworkingsockets.h>
#include "netcode/netcode_common.hpp"
#include "netcode/client_netcode.hpp"
#include "netcode/client_window.hpp"
#include "netcode/valve_sockets_session.hpp"
#include "OpenGL/IGameRenderer.hpp"
#include <memory>
#include <string>
#include <chrono>
#include <thread>
#include <cstdint>
#include <cmath>    // for std::ceil
#include "Utils/Debug/Debug.hpp"

#include "Client-Server/Client.hpp"
#include "Client-Server/InputDelayCalculator.hpp"

#include "NetTFG_Engine.hpp"



class OnlineClient : public Client {
public:
    OnlineClient(std::unique_ptr<IGameLogic> gameLogic,
        std::unique_ptr<IGameRenderer> gameRenderer,std::string binFileName)
        : gameLogic_(std::move(gameLogic))
        , gameRenderer_(std::move(gameRenderer))
        , assignedPlayerId_(-1)
        , isReconnection_(false)
        , serverConnection_(k_HSteamNetConnection_Invalid)
    {
		binName = binFileName;
    }

    ConnectionCode SetupClient(const std::string& hostStr = "0.0.0.0", uint16_t port = 0, const std::string& customClientId = "") override {
        if (!net_.InitGNS()) {
            return CONN_SOCKETS_FAILED;
        }

        clientId_ = customClientId.empty() ? GenerateClientId() : customClientId;

        cWindow_ = new ClientWindow(
            [this](GameStateBlob& state, OpenGLWindow* win) {
                gameRenderer_->Init(state, win);
            },
            [this](GameStateBlob& state, OpenGLWindow* win) {
                gameRenderer_->Render(state, win);
            },
            [this](const GameStateBlob& previousServerState, const GameStateBlob& currentServerState, const GameStateBlob& previousLocalState, const GameStateBlob& currentLocalState, GameStateBlob& renderState, float serverInterpolation, float localInterpolation) {
                gameRenderer_->Interpolate(previousServerState, currentServerState, previousLocalState, currentLocalState, renderState, serverInterpolation, localInterpolation);
            }
        );

        ConnectionCode connCode = net_.ConnectTo(hostStr, port);

        if (connCode != CONN_SUCCESS) {
            return connCode;
        }

        Debug::Info("OnlineClient") << "Using client ID: " << clientId_ << "\n";
        Debug::Info("OnlineClient") << "Waiting for connection to server...\n";

        if (!WaitForConnectionAndAuth()) {
            return CONN_DENIED;
        }

        if (isReconnection_) {
            Debug::Info("OnlineClient") << "Successfully reconnected!\n";
        }
        else {
            Debug::Info("OnlineClient") << "Successfully authenticated!\n";

            if (!WaitForGameStart()) {
                return CONN_TIMEOUT;
            }
        }

        Debug::Info("OnlineClient") << "Before creating prediction netcode\n";

        gameRenderer_->playerId = assignedPlayerId_;

        prediction_ = new ClientPredictionNetcode(assignedPlayerId_, std::move(gameLogic_));

        if (isReconnection_)
        {
            Debug::Info("OnlineClient") << "Waiting for state update after reconnection...\n";
            if (!WaitForStateUpdateAfterReconnection(*prediction_)) {
                return CONN_TIMEOUT;
            }
        }

        cWindow_->activate();

		networkRunning_.store(true);
        networkThread_ = std::thread([this]() {
            NetworkThread(*prediction_, *cWindow_, networkRunning_);
            });

        prediction_->UpdateCurrentFrame(1);

        Debug::Info("OnlineClient") << "Online Client setup OK\n";

        return CONN_SUCCESS;
    }
    

    void TickClient() override {
        

        InputBlob localInput = prediction_->GetGameLogic()->GenerateLocalInput();
        int frameToSubmit = prediction_->SubmitLocalInput(localInput);
        net_.SendInput(serverConnection_, assignedPlayerId_, frameToSubmit, localInput);

        // Predict to current frame (mutex-protected)
        prediction_->Tick();
        cWindow_->setLocalState(prediction_->GetCurrentState());

        // ===== Debug output every 30 frames =====
        if (frameToSubmit % 30 == 0) {
            GameStateBlob s = prediction_->GetCurrentState();
            Debug::Info("OnlineClient") << "[CLIENT] Frame: " << frameToSubmit
                << " | Latency: " << inputDelayCalc.GetLastLatencyMs()
                << "ms | InputDelayFrames: " << inputDelayCalc.GetInputDelayFrames() << "\n";

            // Send RTT sync
            InputDelayPacket packet;
            packet.playerId = assignedPlayerId_;
            packet.timestamp = inputDelayCalc.GetTimestampMs();
            net_.SendInputDelaySync(serverConnection_, packet);
        }

        
    }

	void CloseClient() override {
        // Clean shutdown
        networkRunning_.store(false);
        networkThread_.join();

        cWindow_->deactivate();

		delete cWindow_;
		delete prediction_;

        if (serverConnection_ != k_HSteamNetConnection_Invalid) {
            net_.GetSockets()->CloseConnection(serverConnection_,
                k_ESteamNetConnectionEnd_App_Generic, nullptr, false);
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        Debug::Info("OnlineClient") << "[ONLINE] Online client finalished\n";
	}

    const std::string& GetClientId() const { return clientId_; }

private:
    GNSSession net_;
    InputDelayCalculator inputDelayCalc;
    std::unique_ptr<IGameLogic> gameLogic_;
    std::unique_ptr<IGameRenderer> gameRenderer_;
    std::string clientId_;
    int assignedPlayerId_;
    bool isReconnection_;
    HSteamNetConnection serverConnection_;

	ClientPredictionNetcode* prediction_ = nullptr;
	ClientWindow* cWindow_ = nullptr;

    std::atomic<bool> networkRunning_;
    std::thread networkThread_;

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

        ISteamNetworkingSockets* sockets = net_.GetSockets();
        if (!sockets || serverConnection_ == k_HSteamNetConnection_Invalid) {
            Debug::Info("OnlineClient") << "Failed to send client hello: invalid connection\n";
            return false;
        }

        sockets->SendMessageToConnection(serverConnection_, &hello,
            sizeof(hello), k_nSteamNetworkingSend_Reliable, nullptr);

        Debug::Info("OnlineClient") << "Sent CLIENT_HELLO with ID: " << clientId_ << "\n";
        return true;
    }

	bool WaitForStateUpdateAfterReconnection(ClientPredictionNetcode& prediction) {
		bool stateReceived = false;
		bool running = true;
		auto timeout = std::chrono::steady_clock::now() + std::chrono::seconds(10);
		while (!stateReceived && running) {
			if (std::chrono::steady_clock::now() > timeout) {
				Debug::Info("OnlineClient") << "Timeout waiting for state update after reconnection\n";
				return false;
			}
			net_.PumpCallbacks();
			net_.Poll([&](const uint8_t* data, int len, HSteamNetConnection conn) {
				if (len < 1) {
					return;
				}
				uint8_t type = data[0];
				if (type == PACKET_STATE_UPDATE) {
					StateUpdate update = net_.ParseStateUpdate(data, len);
					prediction.OnServerStateUpdate(update);
					stateReceived = true;
					Debug::Info("OnlineClient") << "Received state update after reconnection\n";
				}
				}, true);
			std::this_thread::sleep_for(std::chrono::milliseconds(50));
		}
		return running && stateReceived;
	}

    bool HandleServerAccept(const uint8_t* data, int len) {
        if (len < sizeof(ServerAcceptPacket)) {
            Debug::Info("OnlineClient") << "Malformed SERVER_ACCEPT packet\n";
            return false;
        }

        const ServerAcceptPacket* accept = (const ServerAcceptPacket*)data;
        assignedPlayerId_ = accept->playerId;
        isReconnection_ = accept->isReconnection;

        if (isReconnection_) {
            Debug::Info("OnlineClient") << "Reconnected as Player ID: " << assignedPlayerId_ << "\n";
        }
        else {
            Debug::Info("OnlineClient") << "Assigned Player ID: " << assignedPlayerId_ << "\n";
        }

        return true;
    }

    bool HandleServerReject(const uint8_t* data, int len) {
        Debug::Info("OnlineClient") << "Server rejected connection\n";
        return false;
    }

    bool HandleGameStart(const uint8_t* data, int len, bool& gameStarted) {
        if (len < 1 + 4) {
            Debug::Info("OnlineClient") << "Malformed GAME_START packet\n";
            return false;
        }

        if (data[0] != PACKET_GAME_START) {
            return false;
        }

        uint32_t playerIdNet = 0;
        std::memcpy(&playerIdNet, data + 1, 4);
        int playerIdFromStart = bigEndianToHost32(playerIdNet);

        if (assignedPlayerId_ != -1 && assignedPlayerId_ != playerIdFromStart) {
            Debug::Info("OnlineClient") << "WARNING: GAME_START player ID mismatch ("
                << playerIdFromStart << " vs " << assignedPlayerId_ << ")\n";
        }

        assignedPlayerId_ = playerIdFromStart;
        gameStarted = true;

        Debug::Info("OnlineClient") << "Game starting with Player ID: " << assignedPlayerId_ << "\n";
        return true;
    }

    bool HandleReceiveEvent(const uint8_t* data, int len, bool& gameStarted, bool& serverAccepted) {
        if (len < 1) {
            return true;
        }

        uint8_t type = data[0];

        if (type == PACKET_SERVER_ACCEPT) {
            if (!HandleServerAccept(data, len)) {
                return false;
            }
            serverAccepted = true;
            return true;
        }

        if (type == PACKET_SERVER_REJECT) {
            return HandleServerReject(data, len);
        }

        if (type == PACKET_GAME_START) {
            if (!HandleGameStart(data, len, gameStarted)) {
                return false;
            }
            return true;
        }

        Debug::Info("OnlineClient") << "Received unknown packet type: " << (int)type << "\n";
        return true;
    }

    bool WaitForConnectionAndAuth() {
        bool connected = false;
        bool serverAccepted = false;
        bool running = true;

        // Wait for connection to establish
        while (!connected && running && ClientWindow::isWindowThreadRunning()) {
            ISteamNetworkingSockets* sockets = net_.GetSockets();
            if (!sockets) {
                running = false;
                break;
            }

            // Check connection state
            serverConnection_ = net_.GetConnectedConnection();
            if (serverConnection_ != k_HSteamNetConnection_Invalid) {
                SteamNetConnectionInfo_t connInfo;
                if (sockets->GetConnectionInfo(serverConnection_, &connInfo)) {
                    if (connInfo.m_eState == k_ESteamNetworkingConnectionState_Connected) {
                        connected = true;
                        Debug::Info("OnlineClient") << "Connected to server\n";

                        if (!SendClientHello()) {
                            running = false;
                        }
                        break;
                    }
                    else if (connInfo.m_eState == k_ESteamNetworkingConnectionState_ClosedByPeer ||
                        connInfo.m_eState == k_ESteamNetworkingConnectionState_Dead) {
                        Debug::Info("OnlineClient") << "Connection to server failed\n";
                        running = false;
                        break;
                    }
                }
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }

        if (!running || !connected || !ClientWindow::isWindowThreadRunning()) {
            return false;
        }

        Debug::Info("OnlineClient") << "Waiting for server acceptance...\n";
        auto timeout = std::chrono::steady_clock::now() + std::chrono::seconds(10);

        while (!serverAccepted && running) {
            if (std::chrono::steady_clock::now() > timeout) {
                Debug::Info("OnlineClient") << "Timeout waiting for server acceptance\n";
                return false;
            }
            net_.PumpCallbacks();
            net_.Poll([&](const uint8_t* data, int len, HSteamNetConnection conn) {
                if (!serverAccepted) {
                    bool gameStarted = false;
                    if (!HandleReceiveEvent(data, len, gameStarted, serverAccepted)) {
                        running = false;
                    }
                }
                }, true);

            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }

        return running && serverAccepted && assignedPlayerId_ != -1;
    }

    bool WaitForGameStart() {
        bool gameStarted = false;
        bool running = true;

        Debug::Info("OnlineClient") << "Waiting for game to start...\n";
        auto timeout = std::chrono::steady_clock::now() + std::chrono::seconds(60);

        while (!gameStarted && running) {
            if (std::chrono::steady_clock::now() > timeout) {
                Debug::Info("OnlineClient") << "Timeout waiting for game start\n";
                return false;
            }

            net_.PumpCallbacks();
            net_.Poll([&](const uint8_t* data, int len, HSteamNetConnection conn) {
                    if (!gameStarted) {
                        bool serverAccepted = false;
                        if (!HandleReceiveEvent(data, len, gameStarted, serverAccepted)) {
                            running = false;
                        }
                    }
                },true);

            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        return running && gameStarted;
    }

    // ============================================================================
    // FIXED ProcessIncomingPacket - frame synchronized
    // ============================================================================

    void ProcessIncomingPacket(ClientPredictionNetcode& prediction,
		ClientWindow& cWin,
        const uint8_t* data,
        int len,
        HSteamNetConnection conn)
    {
        if (len < 1) {
            return;
        }

        uint8_t type = data[0];

        if (type == PACKET_STATE_UPDATE) {
            bool correctPacket = true;
            uint32_t lenVector = 0;
            uint32_t lenState = 0;

            std::memcpy(&lenState, data + 1 + 4, sizeof(uint32_t));
            lenState = bigEndianToHost32(lenState);

            if (len >= 1 + 4 + 4 + lenState) {
                correctPacket = true;
            }

            if (correctPacket) {
                StateUpdate update = net_.ParseStateUpdate(data, len);

                prediction.OnServerStateUpdate(update);
                cWin.setServerState(prediction.GetLatestServerState());
            }
            else {
                Debug::Info("OnlineClient") << "[CLIENT] Received malformed PACKET_STATE_UPDATE, len=" << len << "\n";
            }
        }
        else if (type == PACKET_DELTA_STATE_UPDATE) {
            std::vector<DeltaStateBlob> deltas;
            int frame;

            net_.ParseDeltasUpdate(data, len, deltas, frame);

            prediction.OnServerDeltasUpdate(deltas,frame);
            cWin.setServerState(prediction.GetLatestServerState());
        }
        else if (type == PACKET_INPUT_UPDATE) {
            const size_t EXPECTED_MIN_LEN = 1 + 4 + 4 + sizeof(InputBlob);

            if (len < EXPECTED_MIN_LEN) {
                Debug::Info("OnlineClient") << "[CLIENT] Malformed input packet, len=" << len << "\n";
            }
            else {
                InputEntry ie = net_.ParseInputEntryPacket(data, len);
                prediction.OnServerInputUpdate(ie);
            }
        }
        else if (type == PACKET_EVENT_UPDATE)  {
			const size_t EXPECTED_MIN_LEN = 1 + 4 + 4;
			if (len < EXPECTED_MIN_LEN) {
				Debug::Info("OnlineClient") << "[CLIENT] Malformed event packet, len=" << len << "\n";
			}
			else {
				EventEntry event = net_.ParseEventEntryPacket(data, len);
				prediction.OnServerEventUpdate(event);
			}
        }
        else if (type == PACKET_INPUT_DELAY) {
            // ===== FIX: Validate RTT with bounds checking =====
            InputDelayPacket packet = net_.ParseInputDelaySync(data, len);
            inputDelayCalc.UpdateRtt(packet.timestamp, TICKS_PER_SECOND);

			prediction.UpdateCurrentFrame(inputDelayCalc.GetInputDelayFrames());
        }
    }

    // Network thread function - processes packets directly
    void NetworkThread(ClientPredictionNetcode& prediction,
        ClientWindow& cWindow,
        std::atomic<bool>& running) {
        while (running.load()) {
            net_.PumpCallbacks();

            // Poll and process packets immediately
            // ClientPredictionNetcode's mutex protects shared state
            net_.Poll([&](const uint8_t* data, int len, HSteamNetConnection conn) {
                ProcessIncomingPacket(prediction, cWindow, data, len, conn);
                }, false);

            // Small sleep to prevent busy-waiting (1ms = ~1000 polls/sec)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
};