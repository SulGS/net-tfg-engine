// httplib first: it pulls in winsock2.h, which must precede any windows.h.
#include <httplib.h>
#include <nlohmann/json.hpp>

#include "Matchmaking/MatchmakingClient.hpp"
#include "Utils/PlayerKey.hpp"
#include "Utils/UserDataPath.hpp"

#include <chrono>
#include <condition_variable>
#include <fstream>
#include <thread>

using json = nlohmann::json;

struct MatchmakingSearch {
    std::mutex mutex;
    std::condition_variable cv;
    bool cancelled = false;
    MatchmakingStatus status;
};

namespace
{
    constexpr int MAX_CONSECUTIVE_POLL_FAILURES = 3;

    void SetStatus(MatchmakingSearch& search, const MatchmakingStatus& status) {
        std::lock_guard<std::mutex> lock(search.mutex);
        search.status = status;
    }

    void Fail(MatchmakingSearch& search, const std::string& error) {
        std::lock_guard<std::mutex> lock(search.mutex);
        search.status.state = MatchmakingState::Failed;
        search.status.error = error;
    }

    // "error" field of a JSON error body, or a generic message with the HTTP code.
    std::string ErrorFromResponse(const httplib::Result& res) {
        try {
            json body = json::parse(res->body);
            if (body.contains("error")) {
                return body["error"].get<std::string>();
            }
        }
        catch (const json::exception&) {
        }
        return "Error HTTP " + std::to_string(res->status);
    }

    // Returns true when the search was cancelled while waiting.
    bool WaitOrCancel(MatchmakingSearch& search, std::chrono::milliseconds duration) {
        std::unique_lock<std::mutex> lock(search.mutex);
        return search.cv.wait_for(lock, duration, [&search] { return search.cancelled; });
    }

    void RunSearch(std::shared_ptr<MatchmakingSearch> search, std::string playerName, std::string playerKey,
        MatchmakerAddress address) {
        httplib::Client cli(address.host, address.port);
        cli.set_connection_timeout(3);
        cli.set_read_timeout(5);
        cli.set_write_timeout(5);

        const std::string where = address.host + ":" + std::to_string(address.port);

        json request = {
            { "name", playerName },
            { "key", playerKey },
            { "version", MatchmakingProtocol::VERSION }
        };

        auto res = cli.Post("/queue", request.dump(), "application/json");
        if (!res) {
            Fail(*search, "No se pudo contactar con el matchmaking (" + where + ")");
            return;
        }
        if (res->status != 200) {
            Fail(*search, ErrorFromResponse(res));
            return;
        }

        std::string ticket;
        try {
            ticket = json::parse(res->body).at("ticket").get<std::string>();
        }
        catch (const json::exception&) {
            Fail(*search, "Respuesta inválida del matchmaking");
            return;
        }

        const std::string ticketPath = "/queue/" + ticket;
        MatchmakingStatus status;
        status.state = MatchmakingState::Waiting;
        status.playerName = playerName;
        SetStatus(*search, status);

        int consecutiveFailures = 0;

        while (true) {
            if (WaitOrCancel(*search, std::chrono::milliseconds(MatchmakingProtocol::POLL_INTERVAL_MS))) {
                cli.Delete(ticketPath);
                return;
            }

            res = cli.Get(ticketPath);
            if (!res) {
                if (++consecutiveFailures >= MAX_CONSECUTIVE_POLL_FAILURES) {
                    Fail(*search, "Conexión perdida con el matchmaking (" + where + ")");
                    return;
                }
                continue;
            }
            consecutiveFailures = 0;

            if (res->status == 404) {
                Fail(*search, "El matchmaking ha descartado la búsqueda");
                return;
            }
            if (res->status != 200) {
                Fail(*search, ErrorFromResponse(res));
                return;
            }

            try {
                json body = json::parse(res->body);
                const std::string state = body.at("status").get<std::string>();

                if (state == MatchmakingProtocol::Status::WAITING) {
                    status.state = MatchmakingState::Waiting;
                    status.queued = body.value("queued", 0);
                    status.needed = body.value("needed", 0);
                }
                else if (state == MatchmakingProtocol::Status::STARTING) {
                    status.state = MatchmakingState::Starting;
                }
                else if (state == MatchmakingProtocol::Status::READY) {
                    status.state = MatchmakingState::Ready;
                    status.ip = body.at("ip").get<std::string>();
                    status.port = body.at("port").get<uint16_t>();
                    status.reconnecting = body.value("reconnect", false);
                    SetStatus(*search, status);
                    return;
                }
                else {
                    Fail(*search, body.value("error", std::string("Error en el matchmaking")));
                    return;
                }
            }
            catch (const json::exception&) {
                Fail(*search, "Respuesta inválida del matchmaking");
                return;
            }

            SetStatus(*search, status);
        }
    }
}

bool ParseHostPort(const std::string& text, MatchmakerAddress& out) {
    size_t colon = text.rfind(':');
    if (colon == std::string::npos || colon == 0 || colon + 1 >= text.size()) {
        return false;
    }

    int port = 0;
    try {
        port = std::stoi(text.substr(colon + 1));
    }
    catch (const std::exception&) {
        return false;
    }
    if (port <= 0 || port > 65535) {
        return false;
    }

    out.host = text.substr(0, colon);
    out.port = static_cast<uint16_t>(port);
    return true;
}

MatchmakerAddress LoadMatchmakerAddress() {
    MatchmakerAddress address;
    const std::string path = UserDataPath::File("matchmaking.cfg");

    std::ifstream in(path);
    if (!in.is_open()) {
        std::ofstream out(path);
        out << "host " << address.host << "\n";
        out << "port " << address.port << "\n";
        return address;
    }

    std::string key;
    while (in >> key) {
        if (key == "host") {
            in >> address.host;
        }
        else if (key == "port") {
            int port = 0;
            if (in >> port && port > 0 && port <= 65535) {
                address.port = static_cast<uint16_t>(port);
            }
        }
        else {
            std::string rest;
            std::getline(in, rest);
        }
    }

    return address;
}

MatchmakingClient::~MatchmakingClient() {
    Cancel();
}

void MatchmakingClient::SetServer(const MatchmakerAddress& address) {
    std::lock_guard<std::mutex> lock(mutex_);
    server_ = address;
}

MatchmakerAddress MatchmakingClient::GetServer() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return server_;
}

bool MatchmakingClient::Start(const std::string& playerName) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (search_) {
        std::lock_guard<std::mutex> searchLock(search_->mutex);
        if (search_->status.IsSearching()) {
            return false;
        }
    }

    search_ = std::make_shared<MatchmakingSearch>();
    search_->status.state = MatchmakingState::Queuing;
    search_->status.playerName = playerName;

    // Detached: the thread keeps its own reference to the search, so Cancel() never has to join.
    std::thread(RunSearch, search_, playerName, PlayerKey::ForName(playerName), server_).detach();
    return true;
}

void MatchmakingClient::Cancel() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!search_) {
        return;
    }

    {
        std::lock_guard<std::mutex> searchLock(search_->mutex);
        search_->cancelled = true;
    }
    search_->cv.notify_all();
    search_.reset();
}

void MatchmakingClient::Reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!search_) {
        return;
    }

    bool finished = false;
    {
        std::lock_guard<std::mutex> searchLock(search_->mutex);
        finished = !search_->status.IsSearching();
    }

    // Outside the search lock: this may drop the last reference to its mutex.
    if (finished) {
        search_.reset();
    }
}

MatchmakingStatus MatchmakingClient::GetStatus() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!search_) {
        return {};
    }

    std::lock_guard<std::mutex> searchLock(search_->mutex);
    return search_->status;
}
