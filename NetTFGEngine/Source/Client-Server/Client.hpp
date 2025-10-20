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

class InputDelayCalculator {
public:
    InputDelayCalculator()
        : m_lastRttMs(0),
        m_lastLatencyMs(0.0f),
        m_lastInputDelayFrames(0)
    {
    }

    static uint32_t GetTimestampMs() {
        auto now = std::chrono::high_resolution_clock::now();
        auto ms = std::chrono::time_point_cast<std::chrono::milliseconds>(now);
        uint64_t totalMs = ms.time_since_epoch().count();
        return static_cast<uint32_t>(totalMs & 0xFFFFFFFF);
    }

    void UpdateRtt(uint32_t sentTimestampMs, int tickRate, int logicalTicksDelay) {
        uint32_t now = GetTimestampMs();
        uint32_t rtt = now - sentTimestampMs;

        uint32_t logicalDelayMs = 1000 / tickRate;  // Integer division
        logicalDelayMs *= logicalTicksDelay;
        rtt = (rtt > logicalDelayMs) ? (rtt - logicalDelayMs) : 1;

        // ===== FIX #1: Validate RTT before using it =====
        const uint32_t MAX_REASONABLE_RTT = 10000;  // 10 seconds
        const uint32_t MIN_REASONABLE_RTT = 1;      // 1ms minimum

        if (rtt > MAX_REASONABLE_RTT) {
            std::cerr << "[WARNING] RTT too high: " << rtt
                << "ms (likely clock wrap or packet loss). Ignoring.\n";
            return;
        }

        if (rtt < MIN_REASONABLE_RTT) {
            std::cerr << "[WARNING] RTT suspiciously low: " << rtt
                << "ms. Ignoring.\n";
            return;
        }

        // ===== FIX #2: Use moving average instead of single sample =====
        m_rttSamples.push_back(rtt);
        if (m_rttSamples.size() > RTT_SAMPLE_WINDOW) {
            m_rttSamples.pop_front();
        }

        // Calculate average of all samples in window
        uint32_t sum = 0;
        for (uint32_t sample : m_rttSamples) {
            sum += sample;
        }
        m_lastRttMs = sum / static_cast<uint32_t>(m_rttSamples.size());
        m_lastLatencyMs = m_lastRttMs / 2.0f;

        CalculateInputDelayFrames(TICKS_PER_SECOND);
    }

    // Accessors
    uint32_t GetLastRttMs() const { return m_lastRttMs; }
    float GetLastLatencyMs() const { return m_lastLatencyMs; }
    int GetInputDelayFrames() const { return m_lastInputDelayFrames; }
    uint32_t m_lastRttMs;
    float m_lastLatencyMs;
    int m_lastInputDelayFrames;

    // ===== FIX #2 (continued): Moving average window =====
    static constexpr size_t RTT_SAMPLE_WINDOW = 5;  // Keep last 5 samples
    std::deque<uint32_t> m_rttSamples;

    void CalculateInputDelayFrames(int tickRate) {
        if (tickRate <= 0) return;

        float frameTimeMs = 1000.0f / tickRate;
        m_lastInputDelayFrames = static_cast<int>(std::ceil(m_lastLatencyMs / frameTimeMs));
    }
};



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
        , serverConnection_(k_HSteamNetConnection_Invalid)
    {
    }

    int RunClient(const std::string& hostStr, uint16_t port) {
        if (!net_.InitGNS()) {
            return 1;
        }

        if (!net_.ConnectTo(hostStr, port)) {
            return 1;
        }

        std::cerr << "Using client ID: " << clientId_ << "\n";
        std::cerr << "Waiting for connection to server...\n";

        if (!WaitForConnectionAndAuth()) {
            return 1;
        }

        if (isReconnection_) {
            std::cerr << "Successfully reconnected!\n";
        }
        else {
            std::cerr << "Successfully authenticated!\n";

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
    GNSSession net_;
    InputDelayCalculator inputDelayCalc;
    std::unique_ptr<IGameLogic> gameLogic_;
    std::unique_ptr<IGameRenderer> gameRenderer_;
    std::string clientId_;
    int assignedPlayerId_;
    bool isReconnection_;
    HSteamNetConnection serverConnection_;

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
            std::cerr << "Failed to send client hello: invalid connection\n";
            return false;
        }

        sockets->SendMessageToConnection(serverConnection_, &hello,
            sizeof(hello), k_nSteamNetworkingSend_Reliable, nullptr);

        std::cerr << "Sent CLIENT_HELLO with ID: " << clientId_ << "\n";
        return true;
    }

    bool HandleServerAccept(const uint8_t* data, int len) {
        if (len < sizeof(ServerAcceptPacket)) {
            std::cerr << "Malformed SERVER_ACCEPT packet\n";
            return false;
        }

        const ServerAcceptPacket* accept = (const ServerAcceptPacket*)data;
        assignedPlayerId_ = accept->playerId;
        isReconnection_ = accept->isReconnection;

        if (isReconnection_) {
            std::cerr << "Reconnected as Player ID: " << assignedPlayerId_ << "\n";
        }
        else {
            std::cerr << "Assigned Player ID: " << assignedPlayerId_ << "\n";
        }

        return true;
    }

    bool HandleServerReject(const uint8_t* data, int len) {
        std::cerr << "Server rejected connection\n";
        return false;
    }

    bool HandleGameStart(const uint8_t* data, int len, bool& gameStarted) {
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

        std::cerr << "Received unknown packet type: " << (int)type << "\n";
        return true;
    }

    bool WaitForConnectionAndAuth() {
        bool connected = false;
        bool serverAccepted = false;
        bool running = true;

        // Wait for connection to establish
        while (!connected && running) {
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
                        std::cerr << "Connected to server\n";

                        if (!SendClientHello()) {
                            running = false;
                        }
                        break;
                    }
                    else if (connInfo.m_eState == k_ESteamNetworkingConnectionState_ClosedByPeer ||
                        connInfo.m_eState == k_ESteamNetworkingConnectionState_Dead) {
                        std::cerr << "Connection to server failed\n";
                        running = false;
                        break;
                    }
                }
            }

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
            net_.PumpCallbacks();
            net_.Poll([&](const uint8_t* data, int len, HSteamNetConnection conn) {
                bool gameStarted = false;
                if (!HandleReceiveEvent(data, len, gameStarted, serverAccepted)) {
                    running = false;
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

            net_.PumpCallbacks();
            net_.Poll([&](const uint8_t* data, int len, HSteamNetConnection conn) {
                bool serverAccepted = false;
                if (!HandleReceiveEvent(data, len, gameStarted, serverAccepted)) {
                    running = false;
                }
                });

            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        return running && gameStarted;
    }

    // ============================================================================
    // FIXED ProcessIncomingPacket - frame synchronized
    // ============================================================================

    void ProcessIncomingPacket(ClientPredictionNetcode& prediction,
        int& currentFrame,  // Pass by reference!
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

            if (len >= 1 + 4 + 4 + lenState + 4) {
                uint32_t countRaw = 0;
                std::memcpy(&countRaw, data + 1 + 4 + 4 + lenState, sizeof(uint32_t));
                lenVector = bigEndianToHost32(countRaw);

                size_t expectedLen = 1 + 4 + 4 + lenState + 4 + lenVector * (4 + sizeof(InputBlob));

                if (len != expectedLen) {
                    correctPacket = false;
                }
            }
            else {
                correctPacket = false;
            }

            if (correctPacket) {
                StateUpdate update = net_.ParseStateUpdate(data, len);

                // ===== SYNC: Update frame to match server =====
                if (currentFrame < update.frame) {
                    currentFrame = update.frame;
                    std::cerr << "[SYNC] Synced to server frame: " << currentFrame << "\n";
                }
                prediction.OnServerStateUpdate(update);
            }
            else {
                std::cerr << "[CLIENT] Received malformed PACKET_STATE_UPDATE, len=" << len << "\n";
            }
        }
        else if (type == PACKET_INPUT_UPDATE) {
            const size_t EXPECTED_MIN_LEN = 1 + 4 + 4 + sizeof(InputBlob);

            if (len < EXPECTED_MIN_LEN) {
                std::cerr << "[CLIENT] Malformed input packet, len=" << len << "\n";
            }
            else {
                InputEntry ie = net_.ParseInputEntryPacket(data, len);
                prediction.OnServerInputUpdate(ie);
            }
        }
        else if (type == PACKET_INPUT_DELAY) {
            // ===== FIX: Validate RTT with bounds checking =====
            InputDelayPacket packet = net_.ParseInputDelaySync(data, len);
            std::cout << "Logical ticks delay: " << (packet.recframe - packet.sendframe) << "\n";
            inputDelayCalc.UpdateRtt(packet.timestamp, TICKS_PER_SECOND,packet.recframe - packet.sendframe);
        }
    }

    // ============================================================================
    // FIXED RunClientLoop - proper frame management with sync
    // ============================================================================

    void RunClientLoop(ClientPredictionNetcode& prediction, ClientWindow& cWindow) {
        int currentFrame = 0;  // SINGLE frame counter - shared between local and server
        auto nextTick = std::chrono::high_resolution_clock::now();

        std::thread renderThread(&ClientWindow::run, &cWindow);

        while (cWindow.isRunning()) {
            net_.PumpCallbacks();

            // ===== SYNC: Poll and update currentFrame =====
            net_.Poll([&](const uint8_t* data, int len, HSteamNetConnection conn) {
                ProcessIncomingPacket(prediction, currentFrame, data, len, conn);
                });

            // ===== Use currentFrame (now synced with server) =====
            InputBlob localInput = prediction.GetGameLogic()->GenerateLocalInput();

            int frameToSubmit = currentFrame + inputDelayCalc.GetInputDelayFrames();
            prediction.SubmitLocalInput(frameToSubmit, localInput);
            net_.SendInput(serverConnection_, assignedPlayerId_, frameToSubmit, localInput);

            // Predict to current frame
            prediction.PredictToFrame(currentFrame);
            cWindow.setRenderState(prediction.GetCurrentState());

            // ===== Debug output every 30 frames =====
            if (currentFrame % 30 == 0) {
                GameStateBlob s = prediction.GetCurrentState();
                std::cout << "[CLIENT] Frame: " << currentFrame
                    << " | Latency: " << inputDelayCalc.GetLastLatencyMs()
                    << "ms | InputDelayFrames: " << inputDelayCalc.GetInputDelayFrames() << "\n";

                // Send RTT sync
                InputDelayPacket packet;

                packet.sendframe = currentFrame;
                packet.recframe = currentFrame;
                packet.playerId = assignedPlayerId_;
                packet.timestamp = inputDelayCalc.GetTimestampMs();

                net_.SendInputDelaySync(serverConnection_, packet);
            }

            ++currentFrame;

            nextTick += std::chrono::milliseconds(MS_PER_TICK);
            std::this_thread::sleep_until(nextTick);

            if (currentFrame > 200000) {
                cWindow.close();
            }
        }

        renderThread.join();

        if (serverConnection_ != k_HSteamNetConnection_Invalid) {
            std::cout << "[CLIENT] Window closed, sending disconnect..." << std::endl;
            net_.GetSockets()->CloseConnection(serverConnection_,
                k_ESteamNetConnectionEnd_App_Generic, nullptr, false);
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
};