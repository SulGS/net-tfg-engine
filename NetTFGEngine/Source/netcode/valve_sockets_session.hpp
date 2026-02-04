#ifndef GNS_SESSION_H
#define GNS_SESSION_H

#include "netcode_common.hpp"
#include <GameNetworkingSockets/steam/steamnetworkingtypes.h>
#include <GameNetworkingSockets/steam/steamnetworkingsockets.h>
#include <queue>
#include <functional>
#include "Utils/Debug/Debug.hpp"

enum ConnectionCode : uint8_t {
	CONN_SUCCESS = 0,
	CONN_SOCKETS_FAILED = 1,
	CONN_PARSE_ERROR = 2,
	CONN_TIMEOUT = 3,
	CONN_DENIED = 4
};

std::ostream& operator<<(std::ostream& os, ConnectionCode code) {
    switch (code) {
    case CONN_SUCCESS: return os << "CONN_SUCCESS";
    case CONN_SOCKETS_FAILED: return os << "CONN_SOCKETS_FAILED";
    case CONN_PARSE_ERROR: return os << "CONN_PARSE_ERROR";
    case CONN_TIMEOUT: return os << "CONN_TIMEOUT";
    case CONN_DENIED: return os << "CONN_DENIED";
    default: return os << "UNKNOWN(" << static_cast<int>(code) << ")";
    }
}

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
        if (socketsInitialized) {
            Debug::Info("Sockets") << "GameNetworkingSockets already initialized\n";
            return true;
        }

        SteamNetworkingErrMsg errMsg;
        if (!GameNetworkingSockets_Init(nullptr, errMsg)) {
            Debug::Error("Sockets") << "GameNetworkingSockets initialization failed: " << errMsg << "\n";
            return false;
        }

        socketsInitialized = true;  // mark static flag
        sockets = SteamNetworkingSockets();
        if (!sockets) {
            Debug::Error("Sockets") << "Failed to get SteamNetworkingSockets interface\n";
            return false;
        }

        return true;
    }

    void SetConnectionStateCallback(ConnectionCallback cb) {
        onConnectionStateChanged = cb;
    }

    bool InitHost(uint16_t port) {
        if (!sockets) {
            Debug::Error("Sockets") << "GNS not initialized\n";
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
            Debug::Error("Sockets") << "Failed to create listen socket on port " << port << "\n";
            return false;
        }

        pollGroup = sockets->CreatePollGroup();
        if (pollGroup == k_HSteamNetPollGroup_Invalid) {
            Debug::Error("Sockets") << "Failed to create poll group\n";
            return false;
        }

        // Store this instance for the static callback
        s_pInstance = this;

        isServer = true;
        Debug::Info("Sockets") << "Server listening on port " << port << "\n";
        return true;
    }

    ConnectionCode ConnectTo(const std::string& hostStr, uint16_t port)
    {
        if (!sockets)
            return CONN_SOCKETS_FAILED;

        SteamNetworkingIPAddr serverAddr;
        serverAddr.Clear();
        if (!serverAddr.ParseString(hostStr.c_str()))
            return CONN_PARSE_ERROR;

        serverAddr.m_port = port;

        SteamNetworkingConfigValue_t opt;
        opt.SetInt32(k_ESteamNetworkingConfig_TimeoutInitial, 10000);

        HSteamNetConnection conn =
            sockets->ConnectByIPAddress(serverAddr, 1, &opt);

        if (conn == k_HSteamNetConnection_Invalid)
            return CONN_SOCKETS_FAILED;

        const uint64 start = SteamNetworkingUtils()->GetLocalTimestamp();
        const uint64 timeoutUS = 10 * 1000 * 1000; // 10 seconds

        while (true)
        {
            // REQUIRED: pump callbacks
			PumpCallbacks();

            SteamNetConnectionInfo_t info;
            sockets->GetConnectionInfo(conn, &info);

            switch (info.m_eState)
            {
            case k_ESteamNetworkingConnectionState_Connected:
                connectedConnection = conn;
                return CONN_SUCCESS;

            case k_ESteamNetworkingConnectionState_ProblemDetectedLocally:
            case k_ESteamNetworkingConnectionState_ClosedByPeer:
                sockets->CloseConnection(conn, 0, nullptr, false);
                return CONN_TIMEOUT;

            default:
                break;
            }

            // User-level timeout safety net
            if (SteamNetworkingUtils()->GetLocalTimestamp() - start > timeoutUS)
            {
                sockets->CloseConnection(conn, 0, "User timeout", false);
                return CONN_TIMEOUT;
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }


    void PumpCallbacks() {
        if (sockets) {
            sockets->RunCallbacks();
        }
    }

    void Poll(const std::function<void(const uint8_t*, int, HSteamNetConnection)>& handler, bool fetchOnlyOne) {
        if (!sockets) return;

        ISteamNetworkingMessage* pMsgs[64];
        int numMsgs;
		int messagesToFetch = fetchOnlyOne ? 1 : 64;

        if (isServer && pollGroup != k_HSteamNetPollGroup_Invalid) {
            numMsgs = sockets->ReceiveMessagesOnPollGroup(pollGroup, pMsgs, messagesToFetch);
        }
        else if (!isServer && connectedConnection != k_HSteamNetConnection_Invalid) {
            numMsgs = sockets->ReceiveMessagesOnConnection(connectedConnection, pMsgs, messagesToFetch);
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

	void SendEventUpdate(HSteamNetConnection conn, const EventEntry& event) {
		if (!sockets || conn == k_HSteamNetConnection_Invalid) return;
		size_t bufSize = 1 + 4 + 4 + 4 + event.event.len;
		std::vector<uint8_t> buf(bufSize);
		size_t offset = 0;
		buf[offset++] = PACKET_EVENT_UPDATE;
		uint32_t f = hostToBigEndian32(event.frame);
		std::memcpy(&buf[offset], &f, 4);
		offset += 4;

		uint32_t eventType = hostToBigEndian32(event.event.type);
		std::memcpy(&buf[offset], &eventType, 4);
		offset += 4;

		uint32_t eventLen = hostToBigEndian32(event.event.len);
		std::memcpy(&buf[offset], &eventLen, 4);
		offset += 4;
		std::memcpy(&buf[offset], event.event.data, event.event.len);
		offset += event.event.len;
		sockets->SendMessageToConnection(conn, buf.data(), buf.size(), k_nSteamNetworkingSend_Reliable, nullptr);
	}

	EventEntry ParseEventEntryPacket(const uint8_t* buf, size_t len) {
		EventEntry eventEntry;
		size_t offset = 0;
		offset += 1;
		uint32_t f = 0;
		std::memcpy(&f, buf + offset, 4);
		eventEntry.frame = bigEndianToHost32(f);
		offset += 4;

		uint32_t eventType = 0;
		std::memcpy(&eventType, buf + offset, 4);
		eventEntry.event.type = bigEndianToHost32(eventType);
		offset += 4;

		uint32_t eventLen = 0;
		std::memcpy(&eventLen, buf + offset, 4);
		eventLen = bigEndianToHost32(eventLen);
		offset += 4;
		eventEntry.event.len = eventLen;
		std::memcpy(eventEntry.event.data, buf + offset, eventLen);
		offset += eventLen;
		return eventEntry;
	}

    void SendStateUpdate(HSteamNetConnection conn, const StateUpdate& update) {
        if (!sockets || conn == k_HSteamNetConnection_Invalid) return;

        size_t bufSize = 1 + 4 + 4 + update.state.len;

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

        return update;
    }

    // type(1) + frame(4) + num of deltas(4) + N * delta
    void SendDeltasUpdate(HSteamNetConnection conn, const std::vector<DeltaStateBlob>& deltas, const int frame) {
        if (!sockets || conn == k_HSteamNetConnection_Invalid) return;

        size_t bufSize = 1 + 4 + 4;

        for (const DeltaStateBlob& delta : deltas) {
            bufSize += 4;         // delta_type
            bufSize += 4;         // delta.len
            bufSize += delta.len; // delta payload
        }

        std::vector<uint8_t> buf(bufSize);

        size_t offset = 0;

        buf[offset++] = PACKET_DELTA_STATE_UPDATE;

        uint32_t f = hostToBigEndian32(frame);
        std::memcpy(&buf[offset], &f, 4);
        offset += 4;

        uint32_t numDeltas = hostToBigEndian32(deltas.size());
        std::memcpy(&buf[offset], &numDeltas, 4);
        offset += 4;

        for (const DeltaStateBlob& delta : deltas) 
        {
            uint32_t type = hostToBigEndian32(delta.delta_type);
            std::memcpy(&buf[offset], &type, 4);
            offset += 4;

            uint32_t deltaLen = hostToBigEndian32(delta.len);
            std::memcpy(&buf[offset], &deltaLen, 4);
            offset += 4;

            std::memcpy(&buf[offset], delta.data, delta.len);
            offset += delta.len;
        }

        sockets->SendMessageToConnection(conn, buf.data(), buf.size(), k_nSteamNetworkingSend_Reliable, nullptr);
    }

    void ParseDeltasUpdate(const uint8_t* buf, size_t len, std::vector<DeltaStateBlob>& deltas, int& frame)
    {
        deltas.clear();
        size_t offset = 0;

        // 1) Read packet type
        if (offset + 1 > len) return;
        uint8_t packetType = buf[offset++];
        if (packetType != PACKET_DELTA_STATE_UPDATE)
            return;

        // 2) Read first frame
        if (offset + 4 > len) return;
        uint32_t frameBE;
        std::memcpy(&frameBE, &buf[offset], 4);
        offset += 4;
        frame = bigEndianToHost32(frameBE);

        // 3) Read number of deltas
        if (offset + 4 > len) return;
        uint32_t numDeltasBE;
        std::memcpy(&numDeltasBE, &buf[offset], 4);
        offset += 4;
        uint32_t numDeltas = bigEndianToHost32(numDeltasBE);

        deltas.reserve(numDeltas);

        // 4) Read each delta
        for (uint32_t i = 0; i < numDeltas; i++)
        {
            DeltaStateBlob d{};

            d.frame = frame;

            // delta_type
            if (offset + 4 > len) return;
            uint32_t typeBE;
            std::memcpy(&typeBE, &buf[offset], 4);
            offset += 4;
            d.delta_type = bigEndianToHost32(typeBE);

            // delta_len
            if (offset + 4 > len) return;
            uint32_t deltaLenBE;
            std::memcpy(&deltaLenBE, &buf[offset], 4);
            offset += 4;
            d.len = bigEndianToHost32(deltaLenBE);

            // data pointer allocation
            if (offset + d.len > len) return;
            std::memcpy(d.data, &buf[offset], d.len);
            offset += d.len;

            deltas.push_back(d);
        }
    }

    void SendHashPacket(HSteamNetConnection conn, const HashPacket packet, const int frame) {
        if (!sockets || conn == k_HSteamNetConnection_Invalid) return;

		uint8_t buf[1 + 4 + SHA256_DIGEST_LENGTH];

		buf[0] = PACKET_HASH;
		uint32_t f = hostToBigEndian32(frame);
		std::memcpy(buf + 1, &f, sizeof(uint32_t));
		std::memcpy(buf + 5, packet.hash, SHA256_DIGEST_LENGTH);
		sockets->SendMessageToConnection(conn, buf, sizeof(buf), k_nSteamNetworkingSend_Reliable, nullptr);
    }

	HashPacket ParseHashPacket(const uint8_t* buf, size_t len) {
		HashPacket packet;

		size_t offset = 0;
		offset += 1;
		uint32_t f = 0;
		std::memcpy(&f, buf + offset, 4);
		packet.frame = bigEndianToHost32(f);
		offset += 4;
		std::memcpy(packet.hash, buf + offset, SHA256_DIGEST_LENGTH);
		offset += SHA256_DIGEST_LENGTH;

		return packet;
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
                Debug::Error("Sockets") << "Warning: Failed to add connection to poll group\n";
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
                Debug::Info("Sockets") << "New connection attempt from " << pInfo->m_info.m_szConnectionDescription << "\n";
                if (sockets->AcceptConnection(pInfo->m_hConn) == k_EResultOK) {
                    Debug::Info("Sockets") << "Connection accepted, waiting to add to poll group...\n";
                }
                else {
                    Debug::Error("Sockets") << "Failed to accept connection\n";
                    sockets->CloseConnection(pInfo->m_hConn, 0, nullptr, false);
                }
                break;

            case k_ESteamNetworkingConnectionState_Connected:
                Debug::Info("Sockets") << "Connection now established, adding to poll group\n";
                if (!sockets->SetConnectionPollGroup(pInfo->m_hConn, pollGroup)) {
                    Debug::Error("Sockets") << "Failed to set poll group\n";
                    sockets->CloseConnection(pInfo->m_hConn, 0, nullptr, false);
                }
                break;

            case k_ESteamNetworkingConnectionState_ClosedByPeer:
                Debug::Info("Sockets") << "Connection closed by peer: " << pInfo->m_info.m_szEndDebug << "\n";
                sockets->CloseConnection(pInfo->m_hConn, 0, nullptr, false);
                break;

            case k_ESteamNetworkingConnectionState_ProblemDetectedLocally:
                Debug::Error("Sockets") << "Connection problem detected locally: " << pInfo->m_info.m_szEndDebug << "\n";
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

        if (socketsInitialized) {
            GameNetworkingSockets_Kill();
            socketsInitialized = false;  // reset static flag
        }
    }


    ISteamNetworkingSockets* sockets;
    HSteamListenSocket listenSocket;
    HSteamNetPollGroup pollGroup;
    HSteamNetConnection connectedConnection;
    bool isServer;
    ConnectionCallback onConnectionStateChanged;

    static GNSSession* s_pInstance;

    static inline bool socketsInitialized = false;
};

GNSSession* GNSSession::s_pInstance = nullptr;


#endif // GNS_SESSION_H