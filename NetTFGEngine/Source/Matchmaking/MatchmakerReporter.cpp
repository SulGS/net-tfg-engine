// httplib first: it pulls in winsock2.h, which must precede any windows.h.
#include <httplib.h>
#include <nlohmann/json.hpp>

#include "Matchmaking/MatchmakerReporter.hpp"
#include "Utils/Debug/Debug.hpp"

MatchmakerReporter::MatchmakerReporter(std::string host, uint16_t port, int matchId, std::string token)
    : host_(std::move(host))
    , port_(port)
    , matchId_(matchId)
    , token_(std::move(token))
{
}

bool MatchmakerReporter::ReportReady() const {
    return Post("/server/ready");
}

bool MatchmakerReporter::ReportEnded() const {
    return Post("/server/ended");
}

bool MatchmakerReporter::Post(const std::string& path) const {
    httplib::Client cli(host_, port_);
    cli.set_connection_timeout(2);
    cli.set_read_timeout(2);
    cli.set_write_timeout(2);

    nlohmann::json body = {
        { "matchId", matchId_ },
        { "token", token_ }
    };

    auto res = cli.Post(path, body.dump(), "application/json");
    if (!res) {
        Debug::Error("Matchmaker") << "Could not reach matchmaker at " << host_ << ":" << port_
            << " for " << path << "\n";
        return false;
    }
    if (res->status != 200) {
        Debug::Error("Matchmaker") << path << " rejected with HTTP " << res->status << "\n";
        return false;
    }

    Debug::Info("Matchmaker") << "Reported " << path << " for match " << matchId_ << "\n";
    return true;
}
