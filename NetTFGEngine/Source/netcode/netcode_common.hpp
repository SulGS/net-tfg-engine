#ifndef NETCODE_COMMON_H
#define NETCODE_COMMON_H
#include <enet/enet.h>
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
#include <thread>

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
const int INPUT_DELAY_FRAMES = 3;

enum PacketType : enet_uint8 {
    PACKET_INPUT = 0x01,
    PACKET_STATE_UPDATE = 0x02,
    PACKET_INPUT_UPDATE = 0x03,
    PACKET_GAME_START = 0x04,
    PACKET_INPUT_ACK = 0x05
};


struct InputBlob {
    uint8_t data[4];
};

struct InputEntry {
    int frame;
    InputBlob input;
    int playerId;
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
    // Just a raw byte array — trivially copyable
    uint8_t data[4096]; // or dynamic size negotiated per-game

    int len = 0; // actual length of data used
};

static_assert(std::is_trivially_copyable<GameStateBlob>::value,
              "Blob must be trivially copyable");

class IGameLogic {
public:

    int frame = 0;
    int playerId = -1;

    virtual ~IGameLogic() = default;

    // Clone logic object, but not the blob
    virtual std::unique_ptr<IGameLogic> Clone() const = 0;

    virtual InputBlob GenerateLocalInput() = 0;

    virtual void SimulateFrame(GameStateBlob& state, std::map<int, InputEntry> inputs) = 0;

    virtual bool CompareStates(const GameStateBlob& a, const GameStateBlob& b) const = 0;

    // Initialize default state
    virtual void Init(GameStateBlob& state) = 0;

    // (Optional) Debug / render helpers
    virtual void PrintState(const GameStateBlob& state) const = 0;
};

struct StateUpdate {
    int frame;
    GameStateBlob state;
    std::map<int, InputEntry> confirmedInputs;
};

struct SNAPSHOT { int frame = -1; GameStateBlob state; } ;

using InputHistory = std::map<int, std::map<int, InputEntry>>; // frame -> playerId -> input

// Extended peer info to track player identity and connection state
struct PeerInfo {
    ENetPeer* peer;
    int playerId;
    int lastAckedFrame = -1;
    std::string clientId;  // Unique identifier from client
    bool isConnected;
    std::chrono::steady_clock::time_point disconnectTime;
};

inline uint32_t hostToBigEndian32(uint32_t x) {
    return htonl(x);
}

inline uint32_t bigEndianToHost32(uint32_t x) {
    return ntohl(x);
}

#endif // NETCODE_COMMON_H