#ifndef NETCODE_COMMON_H
#define NETCODE_COMMON_H

#include <GameNetworkingSockets/steam/steamnetworkingtypes.h>
#include <GameNetworkingSockets/steam/steamnetworkingsockets.h>
#include <GameNetworkingSockets/steam/isteamnetworkingutils.h>
#include <conio.h>
#include <functional>
#include <chrono>
#include <cstring>
#include <iostream>
#include <map>
#include <mutex>
#include <thread>
#include <vector>
#include <deque>
#include <optional>
#include <atomic>
#include <cstdint>
#include <algorithm>

#if defined(_WIN32) || defined(_WIN64)
#pragma comment(lib, "ws2_32.lib")
#include <winsock2.h>
#else
#include <arpa/inet.h>
#endif

using namespace std::chrono_literals;

const int TICKS_PER_SECOND = 30;
const int MS_PER_TICK = 1000 / TICKS_PER_SECOND;
const int MAX_ROLLBACK_FRAMES = 10;

enum PacketType : uint8_t {
    PACKET_INPUT = 0x01,
    PACKET_STATE_UPDATE = 0x02,
    PACKET_INPUT_UPDATE = 0x03,
    PACKET_GAME_START = 0x04,
    PACKET_INPUT_ACK = 0x05,
    PACKET_INPUT_DELAY = 0x06
};

struct InputBlob {
    uint8_t data[4];
};

struct InputEntry {
    int frame;
    InputBlob input;
    int playerId;
};

struct InputDelayPacket {
    int sendframe;
    int recframe;
    int playerId;
    uint32_t timestamp;
};

inline InputBlob MakeZeroInputBlob() {
    InputBlob blob;
    std::memset(blob.data, 0, sizeof(blob.data));
    return blob;
}

inline bool operator==(const InputBlob& a, const InputBlob& b) {
    return std::memcmp(a.data, b.data, sizeof(a.data)) == 0;
}

inline bool operator!=(const InputBlob& a, const InputBlob& b) {
    return !(a == b);
}

inline bool operator==(const InputEntry& a, const InputEntry& b) {
    return a.frame == b.frame && a.playerId == b.playerId && a.input == b.input;
}

inline bool operator!=(const InputEntry& a, const InputEntry& b) {
    return !(a == b);
}

static_assert(std::is_trivially_copyable<InputBlob>::value,
    "InputBlob must be trivially copyable");

struct GameStateBlob {
    int frame = 0;
    uint8_t data[4096];
    int len = 0;
};

static_assert(std::is_trivially_copyable<GameStateBlob>::value,
    "Blob must be trivially copyable");

class IGameLogic {
public:
    int frame = 0;
    int playerId = -1;
    virtual ~IGameLogic() = default;
    virtual std::unique_ptr<IGameLogic> Clone() const = 0;
    virtual InputBlob GenerateLocalInput() = 0;
    virtual void SimulateFrame(GameStateBlob& state, std::map<int, InputEntry> inputs) = 0;
    virtual bool CompareStates(const GameStateBlob& a, const GameStateBlob& b) const = 0;
    virtual void Init(GameStateBlob& state) = 0;
    virtual void PrintState(const GameStateBlob& state) const = 0;
};

struct StateUpdate {
    int frame;
    GameStateBlob state;
    std::map<int, InputEntry> confirmedInputs;
};

struct SNAPSHOT {
    int frame = -1;
    GameStateBlob state;
};

using InputHistory = std::map<int, std::map<int, InputEntry>>;

// GameNetworkingSockets connection wrapper
// Replaces ENetPeer with HSteamNetConnection
struct PeerInfo {
    HSteamNetConnection connection;  // GameNetworkingSockets handle
    int playerId;
    int lastAckedFrame = -1;
    std::string clientId;
    bool isConnected;
    std::chrono::steady_clock::time_point disconnectTime;
};

// Connection state enum for consistency with Enet workflow
enum class ConnectionState {
    DISCONNECTED = 0,
    CONNECTING = 1,
    CONNECTED = 2,
    DISCONNECTING = 3
};

// Helper to convert between host and network byte order (unchanged, still needed)
inline uint32_t hostToBigEndian32(uint32_t x) {
    return htonl(x);
}

inline uint32_t bigEndianToHost32(uint32_t x) {
    return ntohl(x);
}

#endif // NETCODE_COMMON_H