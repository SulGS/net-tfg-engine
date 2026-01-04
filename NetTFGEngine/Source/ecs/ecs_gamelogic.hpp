#ifndef ECS_GAMELOGIC_H
#define ECS_GAMELOGIC_H

#include "ecs.hpp"
#include "ecs_common.hpp"
#include "Events/ecs_event_processor.hpp"
#include "Deltas/ecs_delta_processor.hpp"
#include "netcode/netcode_common.hpp"
#include "Collisions/CollisionSystem.hpp"
#include "Collisions/BoxCollider2D.hpp"
#include "Collisions/CircleCollider2D.hpp"
#include "Collisions/BoxCollider3D.hpp"
#include "Collisions/SphereCollider3D.hpp"

#include "OpenAL/AudioComponents.hpp"

class IECSGameLogic : public IGameLogic {
protected:
	EventProcessor* eventProcessor;
	DeltaProcessor* deltaProcessor;

public:
    ECSWorld world;

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

	void GenerateDeltas(const GameStateBlob& previousState, const GameStateBlob& newState) override
	{
		deltaProcessor->GenerateDeltas(previousState, newState, this->generatedDeltas);
	}

    bool CompareStateWithDeltas(const GameStateBlob& state, const std::vector<DeltaStateBlob>& deltas) const override {
        return deltaProcessor->CompareDeltas(deltas,state);
    }

	void ApplyDeltasToGameState(GameStateBlob& state, const std::vector<DeltaStateBlob>& deltas) override {
		deltaProcessor->ProcessDeltas(deltas, state);
	}


    virtual void InitECSLogic(GameStateBlob& state) = 0;

    void Init(GameStateBlob& state) override {
		eventProcessor = new EventProcessor(world, isServer);
		deltaProcessor = new DeltaProcessor(isServer);

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
		this->generatedDeltas.clear();
        ProcessEvents(events);
		ProcessInputs(inputs);
        world.Update(isServer, 1 / TICKS_PER_SECOND);
        
        ECSWorld_To_GameState(state);

        if (isServer)
        {
            this->generatedEvents = world.GetEvents();
            world.ClearEvents();
            GenerateDeltas(prevState, state);
        }
    }
};

#endif // ECS_GAMELOGIC_H