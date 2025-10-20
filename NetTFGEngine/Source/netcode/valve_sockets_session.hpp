#ifndef GNS_SESSION_H
#define GNS_SESSION_H

#include "netcode_common.hpp"
#include <GameNetworkingSockets/steam/steamnetworkingtypes.h>
#include <GameNetworkingSockets/steam/steamnetworkingsockets.h>
#include <queue>
#include <functional>

class GNSSession {
public:
    enum ConnectionEventType {
        CONNECTION_STATE_CONNECTING = 0,
        CONNECTION_STATE_CONNECTED = 1,
        CONNECTION_STATE_CLOSED_BY_PEER = 2,
        CONNECTION_STATE_PROBLEM_DETECTED = 3,
        CONNECTION_STATE_NONE = 4
    };

    using ConnectionCallback = std::function<void(HSteamNetConnection, ConnectionEventType)>;

    GNSSession()
        : sockets(nullptr),
        listenSocket(k_HSteamListenSocket_Invalid),
        pollGroup(k_HSteamNetPollGroup_Invalid),
        connectedConnection(k_HSteamNetConnection_Invalid),
        isServer(false),
        onConnectionStateChanged(nullptr) {
    }

    ~GNSSession() {
        Shutdown();
    }

    bool InitGNS() {
        SteamNetworkingErrMsg errMsg;
        if (!GameNetworkingSockets_Init(nullptr, errMsg)) {
            std::cerr << "GameNetworkingSockets initialization failed: " << errMsg << "\n";
            return false;
        }
        sockets = SteamNetworkingSockets();
        if (!sockets) {
            std::cerr << "Failed to get SteamNetworkingSockets interface\n";
            return false;
        }
        return true;
    }

    void SetConnectionStateCallback(ConnectionCallback cb) {
        onConnectionStateChanged = cb;
    }

    bool InitHost(uint16_t port) {
        if (!sockets) {
            std::cerr << "GNS not initialized\n";
            return false;
        }

        SteamNetworkingIPAddr serverAddr;
        serverAddr.Clear();
        serverAddr.m_port = port;

        // Set up callback for connection state changes
        SteamNetworkingConfigValue_t opt;
        opt.SetPtr(k_ESteamNetworkingConfig_Callback_ConnectionStatusChanged,
            (void*)SteamNetConnectionStatusChangedCallback);

        listenSocket = sockets->CreateListenSocketIP(serverAddr, 1, &opt);
        if (listenSocket == k_HSteamListenSocket_Invalid) {
            std::cerr << "Failed to create listen socket on port " << port << "\n";
            return false;
        }

        pollGroup = sockets->CreatePollGroup();
        if (pollGroup == k_HSteamNetPollGroup_Invalid) {
            std::cerr << "Failed to create poll group\n";
            return false;
        }

        // Store this instance for the static callback
        s_pInstance = this;

        isServer = true;
        std::cerr << "Server listening on port " << port << "\n";
        return true;
    }

    bool ConnectTo(const std::string& hostStr, uint16_t port) {
        if (!sockets) {
            std::cerr << "GNS not initialized\n";
            return false;
        }

        SteamNetworkingIPAddr serverAddr;
        serverAddr.Clear();
        if (!serverAddr.ParseString(hostStr.c_str())) {
            std::cerr << "Failed to parse server address: " << hostStr << "\n";
            return false;
        }
        serverAddr.m_port = port;

        SteamNetworkingConfigValue_t opt;
        opt.SetInt32(k_ESteamNetworkingConfig_TimeoutInitial, 10000);

        connectedConnection = sockets->ConnectByIPAddress(serverAddr, 1, &opt);
        if (connectedConnection == k_HSteamNetConnection_Invalid) {
            std::cerr << "Failed to initiate connection to " << hostStr << ":" << port << "\n";
            return false;
        }

        s_pInstance = this;
        isServer = false;
        std::cerr << "Client connecting to " << hostStr << ":" << port << "\n";
        return true;
    }

    void PumpCallbacks() {
        if (sockets) {
            sockets->RunCallbacks();
        }
    }

    void Poll(const std::function<void(const uint8_t*, int, HSteamNetConnection)>& handler) {
        if (!sockets) return;

        ISteamNetworkingMessage* pMsgs[32];
        int numMsgs;

        if (isServer && pollGroup != k_HSteamNetPollGroup_Invalid) {
            numMsgs = sockets->ReceiveMessagesOnPollGroup(pollGroup, pMsgs, 32);
        }
        else if (!isServer && connectedConnection != k_HSteamNetConnection_Invalid) {
            numMsgs = sockets->ReceiveMessagesOnConnection(connectedConnection, pMsgs, 32);
        }

        for (int i = 0; i < numMsgs; i++) {
            handler(static_cast<const uint8_t*>(pMsgs[i]->m_pData),
                pMsgs[i]->m_cbSize, pMsgs[i]->m_conn);
            pMsgs[i]->Release();
        }
    }

    void SendInput(HSteamNetConnection conn, int playerId, int frame, const InputBlob& input) {
        if (!sockets || conn == k_HSteamNetConnection_Invalid) return;

        uint8_t buf[1 + 4 + 4 + sizeof(InputBlob)];
        buf[0] = PACKET_INPUT;
        uint32_t pid = hostToBigEndian32(playerId);
        std::memcpy(buf + 1, &pid, sizeof(uint32_t));
        uint32_t f = hostToBigEndian32(frame);
        std::memcpy(buf + 5, &f, sizeof(uint32_t));
        std::memcpy(buf + 9, input.data, sizeof(InputBlob));

        sockets->SendMessageToConnection(conn, buf, sizeof(buf), k_nSteamNetworkingSend_Reliable, nullptr);
    }

    void SendInputUpdate(HSteamNetConnection conn, int playerId, int frame, const InputBlob& input) {
        if (!sockets || conn == k_HSteamNetConnection_Invalid) return;

        uint8_t buf[1 + 4 + 4 + sizeof(InputBlob)];
        buf[0] = PACKET_INPUT_UPDATE;
        uint32_t pid = hostToBigEndian32(playerId);
        std::memcpy(buf + 1, &pid, sizeof(uint32_t));
        uint32_t f = hostToBigEndian32(frame);
        std::memcpy(buf + 5, &f, sizeof(uint32_t));
        std::memcpy(buf + 9, input.data, sizeof(InputBlob));

        sockets->SendMessageToConnection(conn, buf, sizeof(buf), k_nSteamNetworkingSend_Reliable, nullptr);
    }

    void SendInputDelaySync(HSteamNetConnection conn, InputDelayPacket InpDel_packet) {
        if (!sockets || conn == k_HSteamNetConnection_Invalid) return;

        uint8_t buf[1 + sizeof(InputDelayPacket)];
        uint8_t offset = 0;

        buf[0] = PACKET_INPUT_DELAY;
        offset += 1;
        
        uint32_t f = hostToBigEndian32(InpDel_packet.sendframe);
        std::memcpy(buf + offset, &f, sizeof(uint32_t));
        offset += sizeof(uint32_t);

        f = hostToBigEndian32(InpDel_packet.recframe);
        std::memcpy(buf + offset, &f, sizeof(uint32_t));
        offset += sizeof(uint32_t);

        uint32_t pid = hostToBigEndian32(InpDel_packet.playerId);
        std::memcpy(buf + offset, &pid, sizeof(uint32_t));
        offset += sizeof(uint32_t);

        uint32_t timeSend = hostToBigEndian32(InpDel_packet.timestamp);
        std::memcpy(buf + offset, &timeSend, sizeof(uint32_t));
        offset += sizeof(uint32_t);


        sockets->SendMessageToConnection(conn, buf, sizeof(buf), k_nSteamNetworkingSend_Reliable, nullptr);
    }

    InputDelayPacket ParseInputDelaySync(const uint8_t* buf, size_t len) {

        InputDelayPacket packet;

        size_t offset = 0;

        offset += 1;

        uint32_t f = 0;
        std::memcpy(&f, buf + offset, 4);
        packet.sendframe = bigEndianToHost32(f);
        offset += sizeof(uint32_t);

        std::memcpy(&f, buf + offset, 4);
        packet.recframe = bigEndianToHost32(f);
        offset += sizeof(uint32_t);

        uint32_t pid = 0;
        std::memcpy(&f, buf + offset, 4);
        packet.playerId = bigEndianToHost32(f);
        offset += sizeof(uint32_t);

        uint32_t timeSend = 0;
        std::memcpy(&f, buf + offset, 4);
        packet.timestamp = bigEndianToHost32(f);
        offset += sizeof(uint32_t);



        return packet;
    }

    inline InputEntry ParseInputEntryPacket(const uint8_t* buf, size_t len) {
        InputEntry entry;
        size_t offset = 0;

        offset += 1;

        uint32_t pid = 0;
        std::memcpy(&pid, buf + offset, sizeof(uint32_t));
        entry.playerId = bigEndianToHost32(pid);
        offset += sizeof(uint32_t);

        uint32_t f = 0;
        std::memcpy(&f, buf + offset, sizeof(uint32_t));
        entry.frame = bigEndianToHost32(f);
        offset += sizeof(uint32_t);

        std::memcpy(entry.input.data, buf + offset, sizeof(InputBlob));

        return entry;
    }

    void SendStateUpdate(HSteamNetConnection conn, const StateUpdate& update) {
        if (!sockets || conn == k_HSteamNetConnection_Invalid) return;

        size_t bufSize = 1 + 4 + 4 + update.state.len + 4
            + update.confirmedInputs.size() * (4 + sizeof(InputBlob));

        std::vector<uint8_t> buf(bufSize);
        size_t offset = 0;

        buf[offset++] = PACKET_STATE_UPDATE;

        uint32_t f = hostToBigEndian32(update.frame);
        std::memcpy(&buf[offset], &f, 4);
        offset += 4;

        uint32_t stateLen = hostToBigEndian32(update.state.len);
        std::memcpy(&buf[offset], &stateLen, 4);
        offset += 4;

        std::memcpy(&buf[offset], update.state.data, update.state.len);
        offset += update.state.len;

        uint32_t count = hostToBigEndian32(update.confirmedInputs.size());
        std::memcpy(&buf[offset], &count, 4);
        offset += 4;

        for (const auto& kv : update.confirmedInputs) {
            const auto& ie = kv.second;
            uint32_t pid = hostToBigEndian32(ie.playerId);
            std::memcpy(&buf[offset], &pid, 4);
            offset += 4;
            std::memcpy(&buf[offset], ie.input.data, sizeof(InputBlob));
            offset += sizeof(InputBlob);
        }

        sockets->SendMessageToConnection(conn, buf.data(), buf.size(), k_nSteamNetworkingSend_Reliable, nullptr);
    }

    StateUpdate ParseStateUpdate(const uint8_t* buf, size_t len) {
        StateUpdate update;
        size_t offset = 0;

        offset += 1;

        uint32_t f = 0;
        std::memcpy(&f, buf + offset, 4);
        update.frame = bigEndianToHost32(f);
        offset += 4;

        uint32_t stateLen = 0;
        std::memcpy(&stateLen, buf + offset, 4);
        stateLen = bigEndianToHost32(stateLen);
        offset += 4;
        update.state.len = stateLen;

        std::memcpy(update.state.data, buf + offset, stateLen);
        offset += stateLen;

        uint32_t count = 0;
        std::memcpy(&count, buf + offset, 4);
        count = bigEndianToHost32(count);
        offset += 4;

        update.confirmedInputs.clear();

        for (uint32_t i = 0; i < count; i++) {
            InputEntry ie;
            uint32_t pid = 0;
            std::memcpy(&pid, buf + offset, 4);
            ie.playerId = bigEndianToHost32(pid);
            offset += 4;

            std::memcpy(ie.input.data, buf + offset, sizeof(InputBlob));
            offset += sizeof(InputBlob);

            ie.frame = update.frame;
            update.confirmedInputs[ie.playerId] = ie;
        }

        return update;
    }

    void BroadcastGameStart(HSteamNetConnection conn, int playerId) {
        if (!sockets || conn == k_HSteamNetConnection_Invalid) return;

        uint8_t buf[1 + 4];
        buf[0] = PACKET_GAME_START;
        uint32_t pid = hostToBigEndian32(playerId);
        std::memcpy(buf + 1, &pid, sizeof(uint32_t));

        sockets->SendMessageToConnection(conn, buf, sizeof(buf), k_nSteamNetworkingSend_Reliable, nullptr);
    }

    void AddConnectionToPollGroup(HSteamNetConnection conn) {
        if (sockets && isServer && pollGroup != k_HSteamNetPollGroup_Invalid) {
            if (!sockets->SetConnectionPollGroup(conn, pollGroup)) {
                std::cerr << "Warning: Failed to add connection to poll group\n";
            }
        }
    }

    HSteamListenSocket GetListenSocket() { return listenSocket; }
    HSteamNetConnection GetConnectedConnection() { return connectedConnection; }
    ISteamNetworkingSockets* GetSockets() { return sockets; }
    HSteamNetPollGroup GetPollGroup() { return pollGroup; }

private:
    void OnConnectionStateChanged(SteamNetConnectionStatusChangedCallback_t* pInfo) {
        ConnectionEventType eventType = CONNECTION_STATE_NONE;

        // Determine event type
        switch (pInfo->m_info.m_eState) {
        case k_ESteamNetworkingConnectionState_Connecting:
            eventType = CONNECTION_STATE_CONNECTING;
            break;
        case k_ESteamNetworkingConnectionState_Connected:
            eventType = CONNECTION_STATE_CONNECTED;
            break;
        case k_ESteamNetworkingConnectionState_ClosedByPeer:
            eventType = CONNECTION_STATE_CLOSED_BY_PEER;
            break;
        case k_ESteamNetworkingConnectionState_ProblemDetectedLocally:
            eventType = CONNECTION_STATE_PROBLEM_DETECTED;
            break;
        default:
            eventType = CONNECTION_STATE_NONE;
            break;
        }

        // Notify callback listener
        if (onConnectionStateChanged) {
            onConnectionStateChanged(pInfo->m_hConn, eventType);
        }

        // Server-side: accept connecting clients
        if (isServer) {
            switch (pInfo->m_info.m_eState) {
            case k_ESteamNetworkingConnectionState_Connecting:
                std::cerr << "New connection attempt from " << pInfo->m_info.m_szConnectionDescription << "\n";
                if (sockets->AcceptConnection(pInfo->m_hConn) == k_EResultOK) {
                    std::cerr << "Connection accepted, waiting to add to poll group...\n";
                }
                else {
                    std::cerr << "Failed to accept connection\n";
                    sockets->CloseConnection(pInfo->m_hConn, 0, nullptr, false);
                }
                break;

            case k_ESteamNetworkingConnectionState_Connected:
                std::cerr << "Connection now established, adding to poll group\n";
                if (!sockets->SetConnectionPollGroup(pInfo->m_hConn, pollGroup)) {
                    std::cerr << "Failed to set poll group\n";
                    sockets->CloseConnection(pInfo->m_hConn, 0, nullptr, false);
                }
                break;

            case k_ESteamNetworkingConnectionState_ClosedByPeer:
                std::cerr << "Connection closed by peer: " << pInfo->m_info.m_szEndDebug << "\n";
                sockets->CloseConnection(pInfo->m_hConn, 0, nullptr, false);
                break;

            case k_ESteamNetworkingConnectionState_ProblemDetectedLocally:
                std::cerr << "Connection problem detected locally: " << pInfo->m_info.m_szEndDebug << "\n";
                sockets->CloseConnection(pInfo->m_hConn, 0, nullptr, false);
                break;

            default:
                break;
            }
        }
    }

    static void SteamNetConnectionStatusChangedCallback(SteamNetConnectionStatusChangedCallback_t* pInfo) {
        if (s_pInstance) {
            s_pInstance->OnConnectionStateChanged(pInfo);
        }
    }

    void Shutdown() {
        if (sockets) {
            if (pollGroup != k_HSteamNetPollGroup_Invalid) {
                sockets->DestroyPollGroup(pollGroup);
                pollGroup = k_HSteamNetPollGroup_Invalid;
            }
            if (listenSocket != k_HSteamListenSocket_Invalid) {
                sockets->CloseListenSocket(listenSocket);
                listenSocket = k_HSteamListenSocket_Invalid;
            }
            if (connectedConnection != k_HSteamNetConnection_Invalid) {
                sockets->CloseConnection(connectedConnection, k_ESteamNetConnectionEnd_App_Generic, nullptr, false);
                connectedConnection = k_HSteamNetConnection_Invalid;
            }
        }
        GameNetworkingSockets_Kill();
    }

    ISteamNetworkingSockets* sockets;
    HSteamListenSocket listenSocket;
    HSteamNetPollGroup pollGroup;
    HSteamNetConnection connectedConnection;
    bool isServer;
    ConnectionCallback onConnectionStateChanged;

    static GNSSession* s_pInstance;
};

GNSSession* GNSSession::s_pInstance = nullptr;

#endif // GNS_SESSION_H