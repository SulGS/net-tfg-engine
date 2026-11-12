#pragma once
#include <unordered_map>
#include <memory>
#include "ecs/ecs.hpp"
#include "ecs/Deltas/ecs_iecs_delta_handler.hpp"

class DeltaProcessor {
private:
    std::unordered_map<int, std::unique_ptr<IDeltaHandler>> handlers;
    bool isServer;

public:
    DeltaProcessor(bool iS)
        : isServer(iS) {
    }

    void RegisterHandler(int deltaType, std::unique_ptr<IDeltaHandler> handler) {
        handlers[deltaType] = std::move(handler);
    }

    // Apply deltas received from server (client-side)
    void ProcessDeltas(const std::vector<DeltaStateBlob>& deltas,
        GameStateBlob& currentState) {
        for (const auto& deltaBlob : deltas) {
            auto it = handlers.find(deltaBlob.delta_type);
            if (it != handlers.end()) {
                it->second->Apply(deltaBlob, currentState);
            }
        }
    }

    bool CompareDeltas(const std::vector<DeltaStateBlob>& deltas,
        const GameStateBlob& currentState) {
        for (const auto& deltaBlob : deltas) {
            auto it = handlers.find(deltaBlob.delta_type);
            if (it != handlers.end()) {
                if (!(it->second->Compare(deltaBlob, currentState))) {
                    return false;
                }
            }
        }

        return true;
    }

    // First delta that doesn't match, described by its handler; "" when they all match.
    std::string DescribeMismatch(const std::vector<DeltaStateBlob>& deltas,
        const GameStateBlob& currentState) {
        for (const auto& deltaBlob : deltas) {
            auto it = handlers.find(deltaBlob.delta_type);
            if (it != handlers.end() && !it->second->Compare(deltaBlob, currentState)) {
                std::string what = it->second->DescribeMismatch(deltaBlob, currentState);
                return what.empty() ? "delta type " + std::to_string(deltaBlob.delta_type) : what;
            }
        }
        return "";
    }

    // Generate deltas by comparing states (server-side)
    void GenerateDeltas(const GameStateBlob& prevState,
        const GameStateBlob& currentState,
        std::vector<DeltaStateBlob>& outDeltas) {
        if (!isServer) return;

        for (auto& [deltaType, handler] : handlers) {
            handler->Check(prevState, currentState, outDeltas);
        }
    }
};