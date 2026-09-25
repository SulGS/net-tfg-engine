#pragma once

#include <cstdint>
#include <string>

// HTTP+JSON contract. Client: POST /queue {name,key,version} -> {ticket} (same name+key in a running match = reconnect, other
// key = 409); GET /queue/<ticket> -> status waiting{queued,needed}|starting|ready{ip,port,reconnect}|error; DELETE -> 204.
// GameServer: POST /server/ready|/server/ended {matchId,token} -> 200|403. Debug: GET /status -> {queued,matches,freePorts}.
namespace MatchmakingProtocol
{
    // Bumped on any incompatible change; the matchmaker rejects other versions with 426.
    inline constexpr int VERSION = 2;

    inline constexpr uint16_t DEFAULT_HTTP_PORT = 27000;

    // Client polls its ticket this often; the matchmaker drops tickets not polled for TICKET_TIMEOUT_S.
    inline constexpr int POLL_INTERVAL_MS = 1000;
    inline constexpr int TICKET_TIMEOUT_S = 10;

    // The nickname travels as the GNS client id, so it follows Server::IsValidClientId
    // (alphanumerics, '-' and '_') with a shorter limit that fits the menu field.
    inline constexpr size_t MAX_NAME_LENGTH = 20;

    inline bool IsValidPlayerName(const std::string& name) {
        if (name.empty() || name.size() > MAX_NAME_LENGTH) {
            return false;
        }
        for (char c : name) {
            unsigned char uc = static_cast<unsigned char>(c);
            bool alnum = (uc >= '0' && uc <= '9') || (uc >= 'a' && uc <= 'z') || (uc >= 'A' && uc <= 'Z');
            if (!alnum && c != '-' && c != '_') {
                return false;
            }
        }
        return true;
    }

    // "key" is the player's PlayerKey (Utils/PlayerKey.hpp).

    namespace Status
    {
        inline constexpr const char* WAITING = "waiting";
        inline constexpr const char* STARTING = "starting";
        inline constexpr const char* READY = "ready";
        // Not "ERROR": wingdi.h defines it as a macro.
        inline constexpr const char* FAILURE = "error";
    }
}
