#pragma once

#include <cstdint>
#include <string>

// Used by a matchmaker-launched game server (--matchmaker/--match-id/--token) to report ready and match over.
// Synchronous with short timeouts, failures only logged: the matchmaker also watches the process, so no port leaks.
class MatchmakerReporter {
public:
    MatchmakerReporter(std::string host, uint16_t port, int matchId, std::string token);

    bool ReportReady() const;
    bool ReportEnded() const;

private:
    bool Post(const std::string& path) const;

    std::string host_;
    uint16_t port_;
    int matchId_;
    std::string token_;
};
