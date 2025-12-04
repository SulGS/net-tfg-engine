#ifndef ECS_GAMELOGIC_H
#define ECS_GAMELOGIC_H

#include "ecs.hpp"
#include "ecs_common.hpp"
#include "Events/ecs_event_processor.hpp"
#include "netcode/netcode_common.hpp"
#include "Collisions/CollisionSystem.hpp"
#include "Collisions/BoxCollider2D.hpp"
#include "Collisions/CircleCollider2D.hpp"
#include "Collisions/BoxCollider3D.hpp"
#include "Collisions/SphereCollider3D.hpp"

class IECSGameLogic : public IGameLogic {
protected:
    ECSWorld world;
	EventProcessor* eventProcessor;
public:
    virtual ~IECSGameLogic() 
	{
		delete eventProcessor;
	}

    virtual void ECSWorld_To_GameState(GameStateBlob& state) = 0;
    virtual void GameState_To_ECSWorld(const GameStateBlob& state) = 0;

    virtual void ProcessEvents(std::vector<EventEntry> events) 
    {
		eventProcessor->ProcessEvents(events);
    }

    virtual void ProcessInputs(std::map<int, InputEntry> inputs) 
    {
        auto query = world.GetEntityManager().CreateQuery<Playable>();
        for (auto [entity, play] : query) {
            auto it = inputs.find(play->playerId);
            if (it != inputs.end()) {
                play->input = it->second.input;
            }
            else {
				play->input = MakeZeroInputBlob();
            }
        }
    }

    virtual void InitECSLogic(GameStateBlob& state) = 0;

    void Init(GameStateBlob& state) override {
		eventProcessor = new EventProcessor(world, isServer);
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

	void Synchronize(GameStateBlob& state) override {
		GameState_To_ECSWorld(state);
	}

    void SimulateFrame(GameStateBlob& state, std::vector<EventEntry> events, std::map<int, InputEntry> inputs) override {
        GameStateBlob prevState = state;
		//GameState_To_ECSWorld(state);
        this->generatedEvents.clear();
        ProcessEvents(events);
		ProcessInputs(inputs);
        world.Update(isServer, 1 / TICKS_PER_SECOND);
        if (isServer)
        {
            this->generatedEvents = world.GetEvents();
			world.ClearEvents();
            GenerateDeltas(prevState, state);
        }
        
        ECSWorld_To_GameState(state);
    }
};

#endif // ECS_GAMELOGIC_H