#ifndef ENET_SESSION_H
#define ENET_SESSION_H
#include "netcode_common.hpp"

class ENetSession {
public:
    ENetSession(): host(nullptr), connectedPeer(nullptr) {}
    ~ENetSession() {
        if (host) {
            enet_host_destroy(host);
            host = nullptr;
        }
        connectedPeer = nullptr;
    }

    bool InitHost(uint16_t port) {
        ENetAddress address; 
        address.host = ENET_HOST_ANY; 
        address.port = port;
        host = enet_host_create(&address, 32, 2, 0, 0);
        if (!host) { 
            std::cerr << "Failed to create ENet host\n"; 
            return false; 
        }
        isServer = true;
        std::cerr << "Server listening on port " << port << "\n";
        return true;
    }

    bool ConnectTo(const std::string &hostStr, uint16_t port) {
        ENetAddress address; 
        enet_address_set_host(&address, hostStr.c_str()); 
        address.port = port;
        host = enet_host_create(nullptr, 1, 2, 0, 0);
        if (!host) { 
            std::cerr << "Failed to create ENet client host\n"; 
            return false; 
        }
        ENetPeer* peer = enet_host_connect(host, &address, 2, 0);
        if (!peer) { 
            std::cerr << "No available peers for initiating connection\n"; 
            return false; 
        }
        connectedPeer = peer;
        isServer = false;
        std::cerr << "Client connecting to " << hostStr << ":" << port << "\n";
        return true;
    }

    void Poll(const std::function<void(ENetEvent&)> &handler) {
        ENetEvent event;
        while (enet_host_service(host, &event, 0) > 0) {
            handler(event);
        }
    }

    void SendInput(ENetPeer* peer, int playerId, int frame, const InputBlob& input) {
        uint8_t buf[1 + 4 + 4 + sizeof(InputBlob)];
        buf[0] = PACKET_INPUT;
        uint32_t pid = hostToBigEndian32(playerId);
        std::memcpy(buf + 1, &pid, sizeof(uint32_t));
        uint32_t f = hostToBigEndian32(frame);
        std::memcpy(buf + 5, &f, sizeof(uint32_t));
        std::memcpy(buf + 9, input.data, sizeof(InputBlob));
        ENetPacket* packet = enet_packet_create(buf, sizeof(buf), ENET_PACKET_FLAG_RELIABLE);
        if (peer) enet_peer_send(peer, 0, packet);
    }

    void SendInputUpdate(ENetPeer* peer, int playerId, int frame, const InputBlob& input) {
        uint8_t buf[1 + 4 + 4 + sizeof(InputBlob)];
        buf[0] = PACKET_INPUT_UPDATE;
        uint32_t pid = hostToBigEndian32(playerId);
        std::memcpy(buf + 1, &pid, sizeof(uint32_t));
        uint32_t f = hostToBigEndian32(frame);
        std::memcpy(buf + 5, &f, sizeof(uint32_t));
        std::memcpy(buf + 9, input.data, sizeof(InputBlob));
        ENetPacket* packet = enet_packet_create(buf, sizeof(buf), ENET_PACKET_FLAG_RELIABLE);
        if (peer) enet_peer_send(peer, 0, packet);
    }

    inline InputEntry ParseInputEntryPacket(const uint8_t* buf, size_t len) {
        InputEntry entry;
        size_t offset = 0;

        // Skip packet type
        offset += 1;

        // Player ID
        uint32_t pid = 0;
        std::memcpy(&pid, buf + offset, sizeof(uint32_t));
        entry.playerId = bigEndianToHost32(pid);
        offset += sizeof(uint32_t);

        // Frame
        uint32_t f = 0;
        std::memcpy(&f, buf + offset, sizeof(uint32_t));
        entry.frame = bigEndianToHost32(f);
        offset += sizeof(uint32_t);

        // InputBlob
        std::memcpy(entry.input.data, buf + offset, sizeof(InputBlob));

        return entry;
    }

    void SendStateUpdate(ENetPeer* peer, const StateUpdate& update) {
        // total size = header + frame + state_data + count + each (pid + input)
        size_t bufSize = 1 + 4 + 4 + update.state.len + 4 
                    + update.confirmedInputs.size() * (4 + sizeof(InputBlob));

        std::vector<uint8_t> buf(bufSize);
        size_t offset = 0;

        // Packet type
        buf[offset++] = PACKET_STATE_UPDATE;

        // Frame (network order)
        uint32_t f = hostToBigEndian32(update.frame);
        std::memcpy(&buf[offset], &f, 4);
        offset += 4;

        uint32_t stateLen = update.state.len;
        stateLen = hostToBigEndian32(stateLen);
        std::memcpy(&buf[offset], &stateLen, 4);
        offset += 4;

        // State data (raw bytes only, no "frame" inside)
        std::memcpy(&buf[offset], update.state.data, update.state.len);
        offset += update.state.len;

        // Number of confirmed inputs
        uint32_t count = hostToBigEndian32(update.confirmedInputs.size());
        std::memcpy(&buf[offset], &count, 4);
        offset += 4;

        // Inputs
        for (const auto& kv : update.confirmedInputs) {
            const auto& ie = kv.second;

            // Player ID
            uint32_t pid = hostToBigEndian32(ie.playerId);
            std::memcpy(&buf[offset], &pid, 4);
            offset += 4;

            // Input blob (4 bytes raw)
            std::memcpy(&buf[offset], ie.input.data, sizeof(InputBlob));
            offset += sizeof(InputBlob);
        }

        ENetPacket* packet = enet_packet_create(buf.data(), buf.size(), ENET_PACKET_FLAG_RELIABLE);
        if (peer) enet_peer_send(peer, 0, packet);
    }


    StateUpdate ParseStateUpdate(const uint8_t* buf, size_t len) {
        StateUpdate update;
        size_t offset = 0;

        /*if (len < 1 + 4 + 1024 + 4) {
            throw std::runtime_error("ParseStateUpdate: buffer too small");
        }*/

        // Packet type (skip)
        offset += 1;

        // Frame
        uint32_t f = 0;
        std::memcpy(&f, buf + offset, 4);
        update.frame = bigEndianToHost32(f);
        offset += 4;

        uint32_t stateLen = 0;
        std::memcpy(&stateLen, buf + offset, 4);
        stateLen = bigEndianToHost32(stateLen);
        offset += 4;
        update.state.len = stateLen;

        // State data (raw only)
        std::memcpy(update.state.data, buf + offset, stateLen);
        offset += stateLen;

        // Input count
        uint32_t count = 0;
        std::memcpy(&count, buf + offset, 4);
        count = bigEndianToHost32(count);
        offset += 4;

        update.confirmedInputs.clear();

        for (uint32_t i = 0; i < count; i++) {
            /*if (offset + 4 + sizeof(InputBlob) > len) {
                throw std::runtime_error("ParseStateUpdate: buffer truncated");
            }*/

            InputEntry ie;

            // Player ID
            uint32_t pid = 0;
            std::memcpy(&pid, buf + offset, 4);
            ie.playerId = bigEndianToHost32(pid);
            offset += 4;

            // Input blob
            std::memcpy(ie.input.data, buf + offset, sizeof(InputBlob));
            offset += sizeof(InputBlob);

            // Assign frame from update
            ie.frame = update.frame;

            update.confirmedInputs[ie.playerId] = ie;
        }

        return update;
    }


    void BroadcastGameStart(ENetPeer* peer, int playerId) {
        uint8_t buf[1 + 4];
        buf[0] = PACKET_GAME_START;
        uint32_t pid = hostToBigEndian32(playerId);
        std::memcpy(buf + 1, &pid, sizeof(uint32_t));
        
        ENetPacket* packet = enet_packet_create(buf, sizeof(buf), ENET_PACKET_FLAG_RELIABLE);
        if (peer) enet_peer_send(peer, 0, packet);
    }

    ENetHost* GetHost() { return host; }

private:
    ENetHost* host;
    ENetPeer* connectedPeer;
    bool isServer = false;
};
#endif // ENET_SESSION_H