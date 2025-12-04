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
const int MAX_ROLLBACK_FRAMES = 90;

// Tamaños ajustables según tus necesidades
constexpr size_t GAME_EVENT_BLOB_SIZE = 128;
constexpr size_t STATE_DELTA_BLOB_SIZE = 1024;

// Evento de juego como blob
struct GameEventBlob {
	int type = 0;
    uint8_t data[GAME_EVENT_BLOB_SIZE];
    int len = 0; // longitud real de datos válidos
};

struct EventEntry {
    int frame;
    GameEventBlob event;
};

inline GameEventBlob MakeEmptyGameEventBlob() {
	GameEventBlob blob;
	std::memset(blob.data, 0, sizeof(blob.data));
	blob.len = 0;
	return blob;
}

inline bool operator==(const GameEventBlob& a, const GameEventBlob& b) {
	if (a.len != b.len || a.type != b.type) return false;
	return std::memcmp(a.data, b.data, a.len) == 0;
}

inline bool operator!=(const GameEventBlob& a, const GameEventBlob& b) {
	return !(a == b);
}

inline bool operator==(const EventEntry& a, const EventEntry& b) {
	return a.frame == b.frame && a.event == b.event;
}

inline bool operator!=(const EventEntry& a, const EventEntry& b) {
	return !(a == b);
}

/*
// Delta de estado como blob
struct StateDeltaBlob {
    int fromFrame = 0;
    int toFrame = 0;
	int deltaType = 0;
    uint8_t data[STATE_DELTA_BLOB_SIZE];
    int len = 0;
};
*/

enum PacketType : uint8_t {
    PACKET_INPUT = 0x01,
    PACKET_STATE_UPDATE = 0x02,
    PACKET_INPUT_UPDATE = 0x03,
    PACKET_GAME_START = 0x04,
    PACKET_INPUT_ACK = 0x05,
    PACKET_INPUT_DELAY = 0x06,
	PACKET_DELTA_STATE_UPDATE = 0x07,
	PACKET_EVENT_UPDATE = 0x08
};

struct DeltaStateBlob {
    int frame = 0;
    int delta_type = 0;
    uint8_t data[1024];
    int len = 0;
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
	bool isServer = false;
    int frame = 0;
    int playerId = -1;
	std::vector<EventEntry> generatedEvents;
    std::vector<DeltaStateBlob> generatedDeltas;

    virtual ~IGameLogic() = default;
    virtual std::unique_ptr<IGameLogic> Clone() const = 0;
    virtual InputBlob GenerateLocalInput() = 0;
    virtual void SimulateFrame(GameStateBlob& state, std::vector<EventEntry> events, std::map<int, InputEntry> inputs) = 0;
	virtual void Synchronize(GameStateBlob& state) = 0;
    virtual void GetGeneratedEvents(std::vector<EventEntry>& events) { events = generatedEvents; }
    virtual void GetGeneratedDeltas(std::vector<DeltaStateBlob>& deltas) { deltas = generatedDeltas; }
    virtual bool CompareStates(const GameStateBlob& a, const GameStateBlob& b) const = 0;
    virtual bool CompareStateWithDelta(const GameStateBlob& state, const DeltaStateBlob& delta) const = 0;
    virtual void GenerateDeltas(const GameStateBlob& previousState, const GameStateBlob& newState) = 0;
    virtual void ApplyDeltaToGameState(GameStateBlob& state, const DeltaStateBlob& delta) = 0;
    virtual void Init(GameStateBlob& state) = 0;
    virtual void PrintState(const GameStateBlob& state) const = 0;
};

struct StateUpdate {
    int frame;
    GameStateBlob state;
};

struct Snapshot {
    int frame = -1;
    GameStateBlob state;
    std::map<int, InputEntry> inputs;
	std::vector<EventEntry> events;
    bool stateConfirmed = false;
};

using InputHistory = std::map<int, std::map<int, InputEntry>>;
using EventsHistory = std::map<int, std::vector<EventEntry>>;

// GameNetworkingSockets connection wrapper
// Replaces ENetPeer with HSteamNetConnection
struct PeerInfo {
    HSteamNetConnection connection;  // GameNetworkingSockets handle
    int playerId;
    int lastAckedFrame = -1;
    std::string clientId;
    bool isConnected;
	bool pendingReceiveFullState = true;
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