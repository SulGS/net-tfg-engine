#pragma once
#include "ecs/ecs.hpp"
#include "netcode/netcode_common.hpp"
#include <vector>

class IDeltaHandler {
public:
    virtual ~IDeltaHandler() = default;

    virtual void Apply(const DeltaStateBlob& delta, GameStateBlob& currentState) = 0;

    virtual void Check(const GameStateBlob& prevState,
        const GameStateBlob& currentState,
        std::vector<DeltaStateBlob>& outDeltas) = 0;

    virtual bool Compare(const DeltaStateBlob& delta,
        const GameStateBlob& currentState) = 0;
};