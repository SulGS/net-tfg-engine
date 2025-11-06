#pragma once
#include "ecs/ecs.hpp"
#include "netcode/netcode_common.hpp"

class IEventHandler {
public:
    virtual ~IEventHandler() = default;
    virtual void Handle(const GameEventBlob& event, ECSWorld& world, bool isServer) = 0;
};