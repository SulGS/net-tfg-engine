#pragma once
#include <unordered_map>
#include <memory>
#include "ecs/ecs.hpp"
#include "ecs_iecs_event_handler.hpp"

class EventProcessor {
private:
    std::unordered_map<int, std::unique_ptr<IEventHandler>> handlers;
    ECSWorld& world;
    bool isServer;

public:
    EventProcessor(ECSWorld& w, bool iS)
        : world(w), isServer(iS) {
    }

    void RegisterHandler(int eventType, std::unique_ptr<IEventHandler> handler) {
        handlers[eventType] = std::move(handler);
    }

    void ProcessEvents(const std::vector<EventEntry>& events) {
        for (const auto& eventEntry : events) {
            auto it = handlers.find(eventEntry.event.type);
            if (it != handlers.end()) {
                it->second->Handle(eventEntry.event, world, isServer);
            }
        }
    }
};