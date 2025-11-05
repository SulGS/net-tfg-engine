#ifndef ECS_GAMELOGIC_H
#define ECS_GAMELOGIC_H

#include "ecs.hpp"
#include "ecs_common.hpp"
#include "netcode/netcode_common.hpp"
#include "Collisions/CollisionSystem.hpp"
#include "Collisions/BoxCollider2D.hpp"
#include "Collisions/CircleCollider2D.hpp"
#include "Collisions/BoxCollider3D.hpp"
#include "Collisions/SphereCollider3D.hpp"

class IECSGameLogic : public IGameLogic {
protected:
    ECSWorld world;
public:
    virtual ~IECSGameLogic() = default;
    virtual void ECSWorld_To_GameState(GameStateBlob& state) = 0;
    virtual void GameState_To_ECSWorld(const GameStateBlob& state) = 0;

    virtual void ProcessEvents(std::vector<EventEntry> events) = 0;
    virtual void ProcessInputs(std::map<int, InputEntry> inputs) = 0;

    virtual void InitECSLogic(GameStateBlob& state) = 0;

    void Init(GameStateBlob& state) override {
        world.GetEntityManager().RegisterComponentType<Transform>();
        world.GetEntityManager().RegisterComponentType<Playable>();

        if (isServer) {
            world.GetEntityManager().RegisterComponentType<BoxCollider2D>();
            world.GetEntityManager().RegisterComponentType<CircleCollider2D>();
            world.GetEntityManager().RegisterComponentType<BoxCollider3D>();
            world.GetEntityManager().RegisterComponentType<SphereCollider3D>();
            world.AddSystem(std::make_unique<CollisionSystem>());
        }

        world.AddSystem(std::make_unique<DestroyingSystem>());

        InitECSLogic(state);
    }

    void SimulateFrame(GameStateBlob& state, std::vector<EventEntry> events, std::map<int, InputEntry> inputs) override {
		GameState_To_ECSWorld(state);
        this->generatedEvents.clear();
        ProcessEvents(events);
		ProcessInputs(inputs);
        world.Update(1 / TICKS_PER_SECOND);
        if (isServer)
        {
            this->generatedEvents = world.GetEvents();
        }
        
        ECSWorld_To_GameState(state);
    }
};

#endif // ECS_GAMELOGIC_H