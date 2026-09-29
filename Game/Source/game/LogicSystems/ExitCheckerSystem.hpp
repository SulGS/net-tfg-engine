#pragma once

#include "NetTFG_Engine.hpp"
#include "Utils/Debug/Debug.hpp"
#include "ecs/ecs.hpp"
#include "../Components.hpp"

class ExitCheckerSystem : public ISystem {
public:
    void Update(EntityManager& entityManager, std::vector<EventEntry>& events, bool isServer, float deltaTime) override {
        if (isServer) return;
        if (switchRequested_) return;  // only fire once

        auto query = entityManager.CreateQuery<ExitButtonChecker>();
        for (auto [entity, checker] : query)
        {
            if (checker->exitPressed)
            {
                switchRequested_ = true;
                Debug::Info("Asteroids") << "Exit button pressed, switching back to lobby...\n";
                // Deactivate 1 FIRST, then activate 0 — handled atomically by the engine loop
                NetTFG_Engine::Get().ActivateClientAsync(0,
                    [](int id, ConnectionCode code) {
                        if (code == CONN_SUCCESS)
                        {
                            Debug::Info("Asteroids") << "Switched back to client " << id << "\n";
                            // Deferred (not DeactivateClient) since client 1 may still be mid-tick on this background thread.
                            NetTFG_Engine::Get().RequestDeactivateClient(1);
                        }
                        else
                            Debug::Error("Asteroids") << "Failed to switch to client " << id << ": " << code << "\n";
                    });
            }
        }
    }
private:
    bool switchRequested_ = false;
};
