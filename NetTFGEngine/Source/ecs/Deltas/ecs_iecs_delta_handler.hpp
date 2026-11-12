#pragma once
#include "ecs/ecs.hpp"
#include "netcode/netcode_common.hpp"
#include <vector>
#include <string>

class IDeltaHandler {
public:
    virtual ~IDeltaHandler() = default;

    virtual void Apply(const DeltaStateBlob& delta, GameStateBlob& currentState) = 0;

    virtual void Check(const GameStateBlob& prevState,
        const GameStateBlob& currentState,
        std::vector<DeltaStateBlob>& outDeltas) = 0;

    virtual bool Compare(const DeltaStateBlob& delta,
        const GameStateBlob& currentState) = 0;

    // Human-readable first difference found by Compare, for the reconciliation log ("" = not implemented).
    virtual std::string DescribeMismatch(const DeltaStateBlob& delta, const GameStateBlob& currentState) const { return ""; }
};