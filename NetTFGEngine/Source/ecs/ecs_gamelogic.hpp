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
	EventProcessor* eventProcessor = nullptr;
	DeltaProcessor* deltaProcessor = nullptr;
	std::map<int, InputBlob> lastKnownInput;

public:
    ECSWorld world;

    virtual ~IECSGameLogic() 
	{
		delete eventProcessor;
		delete deltaProcessor;
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
                lastKnownInput[play->playerId] = it->second.input;
            }
            else {
                // No packet arrived for this player this frame (late/lost,
                // or outrun by the server's fixed tick timer). Repeat their
                // last known input instead of zero-filling: a zero-fill
                // reads as the key being released for exactly this frame,
                // which can swallow a still-held SHOOT press right as a
                // charge finishes — the client's local prediction already
                // played the charge, so nothing looks wrong until the
                // bullet silently fails to spawn.
                auto lastIt = lastKnownInput.find(play->playerId);
                play->input = lastIt != lastKnownInput.end() ? lastIt->second : MakeZeroInputBlob();
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
        world.Reset();
        lastKnownInput.clear();

        delete eventProcessor;
		delete deltaProcessor;

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

    // Destroys every component in this world, which drops any AssetManager
    // ref-counts they hold (see ecs.hpp EntityManager::Reset). Safe to call
    // again from the next Init(), which resets the world unconditionally.
    void ReleaseECSAssets() override {
        world.Reset();
    }

	void Synchronize(GameStateBlob& state) override {
		GameState_To_ECSWorld(state);
	}

    void SimulateFrame(GameStateBlob& state, std::vector<EventEntry> events, std::map<int, InputEntry> inputs) override {
        GameStateBlob prevState;
        ECSWorld_To_GameState(prevState);
        this->generatedEvents.clear();
		this->generatedDeltas.clear();

		world.GetEntityManager().acquireMutex();

        ProcessEvents(events);
		ProcessInputs(inputs);
        gameFinished = world.Update(isServer, 1.0f / TICKS_PER_SECOND);
        
        ECSWorld_To_GameState(state);

        if (isServer)
        {
            this->generatedEvents = world.GetEvents();
            world.ClearEvents();
            GenerateDeltas(prevState, state);
        }

		world.GetEntityManager().releaseMutex();
    }
};

#endif // ECS_GAMELOGIC_H