#ifndef START_SCREEN_GAME
#define START_SCREEN_GAME

#include "OpenGL/OpenGLIncludes.hpp"
#include "netcode/netcode_common.hpp"
#include <memory>
#include <cstring>
#include "Utils/Input.hpp"
#include "OpenGL/IGameRenderer.hpp"
#include <math.h>
#include <cmath>
#include "ecs/ecs.hpp"
#include "ecs/ecs_gamelogic.hpp"
#include "ecs/ecs_common.hpp"
#include "OpenGL/IECSGameRenderer.hpp"
#include "ecs/UI/UIButton.hpp"
#include "ecs/UI/UIImage.hpp"
#include "ecs/UI/UIText.hpp"
#include "ecs/UI/UIElement.hpp"
#include "NetTFG_Engine.hpp"

// Simple game state for start screen
struct StartScreenGameState {
    bool spacePressed;
    int frameCount;
};

enum StartScreenInputMask : uint8_t {
    INPUT_NULL = 0,
    INPUT_SPACE = 1 << 0
};

// System to detect space key press
class StartScreenInputSystem : public ISystem {
public:
    void Update(EntityManager& entityManager, std::vector<EventEntry>& events, bool isServer, float deltaTime) override {
        auto query = entityManager.CreateQuery<Playable>();

        for (auto [entity, play] : query) {
            InputBlob input = play->input;
            uint8_t m = input.data[0];

            if (m & INPUT_SPACE) {
                printf("[StartScreen] SPACE KEY PRESSED! Frame detected.\n");
                NetTFG_Engine::Get().RequestClientSwitch(1,"127.0.0.1",12345);
            }
        }
    }
};

class StartScreenGame : public IECSGameLogic {
public:
    std::unique_ptr<IGameLogic> Clone() const override {
        return std::make_unique<StartScreenGame>();
    }

    InputBlob GenerateLocalInput() override {
        uint8_t m = INPUT_NULL;
        if (Input::KeyPressed(Input::CharToKeycode(' '))) {
            m |= INPUT_SPACE;
        }

        InputBlob buf = MakeZeroInputBlob();
        buf.data[0] = m;
        return buf;
    }

    void GameState_To_ECSWorld(const GameStateBlob& state) {
        StartScreenGameState s = *reinterpret_cast<const StartScreenGameState*>(state.data);

        // Update any ECS components if needed
        // In this simple case, we don't need to sync much
    }

    void ECSWorld_To_GameState(GameStateBlob& state) {
        StartScreenGameState& s = *reinterpret_cast<StartScreenGameState*>(state.data);

        // Increment frame count
        s.frameCount++;

        // Check if space was pressed
        auto query = world.GetEntityManager().CreateQuery<Playable>();
        for (auto [entity, play] : query) {
            InputBlob input = play->input;
            uint8_t m = input.data[0];

            if (m & INPUT_SPACE) {
                s.spacePressed = true;
                printf("[StartScreen] Space pressed in frame %d\n", s.frameCount);
            }
        }

        state.len = sizeof(StartScreenGameState);
    }

    bool CompareStates(const GameStateBlob& a, const GameStateBlob& b) const override {
        return std::memcmp(a.data, b.data, sizeof(StartScreenGameState)) == 0;
    }

    void InitECSLogic(GameStateBlob& state) override {
        StartScreenGameState* s = reinterpret_cast<StartScreenGameState*>(state.data);

        // Initialize state
        s->spacePressed = false;
        s->frameCount = 0;
        state.len = sizeof(StartScreenGameState);

        // Create a player entity to receive input
        Entity player = world.GetEntityManager().CreateEntity();
        world.GetEntityManager().AddComponent<Playable>(player, Playable{ 0, MakeZeroInputBlob(), true });

        // Add input detection system
        world.AddSystem(std::make_unique<StartScreenInputSystem>());

        printf("[StartScreen] Game logic initialized!\n");
    }

    void PrintState(const GameStateBlob& state) const override {
        StartScreenGameState s;
        std::memcpy(&s, state.data, sizeof(StartScreenGameState));
        printf("=== Start Screen State ===\n");
        printf("Space Pressed: %s\n", s.spacePressed ? "YES" : "NO");
        printf("Frame Count: %d\n", s.frameCount);
        printf("==========================\n");
    }
};

class StartScreenGameRenderer : public IECSGameRenderer {
public:
    void GameState_To_ECSWorld(const GameStateBlob& state) {
        StartScreenGameState s = *reinterpret_cast<const StartScreenGameState*>(state.data);

        // Update text if space was pressed
        auto textQuery = world.GetEntityManager().CreateQuery<UIElement, UIText>();
        for (auto [entity, element, text] : textQuery) {
            if (s.spacePressed) {
                text->text = "Space Detected! Starting...";
            }
        }
    }

    void InitECSRenderer(const GameStateBlob& state, OpenGLWindow* window) override {
        this->window = window;

        StartScreenGameState s;
        std::memcpy(&s, state.data, sizeof(StartScreenGameState));

        // Create camera
        Entity camera = world.GetEntityManager().CreateEntity();
        Transform* camTrans = world.GetEntityManager().AddComponent<Transform>(camera, Transform{});
        camTrans->setPosition(glm::vec3(0.0f, 0.0f, 2.0f));

        Camera* camSettings = world.GetEntityManager().AddComponent<Camera>(camera, Camera{});
        camSettings->setOrthographic(-100.0f, 100.0f, -100.0f, 100.0f, 0.1f, 100.0f);
        camSettings->setTarget(glm::vec3(0.0f, 0.0f, 0.0f));
        camSettings->setUp(glm::vec3(0.0f, 1.0f, 0.0f));

        // Create UI text entity
        Entity startText = world.GetEntityManager().CreateEntity();

        UIElement* element = world.GetEntityManager().AddComponent<UIElement>(startText, UIElement{});
        element->anchor = UIAnchor::CENTER;
        element->position = glm::vec2(0.0f, 0.0f);
        element->size = glm::vec2(400.0f, 80.0f);
        element->pivot = glm::vec2(0.5f, 0.5f);
        element->layer = 1;

        UIText* text = world.GetEntityManager().AddComponent<UIText>(startText, UIText{});
        text->text = "Press Space to Start";
        text->fontSize = 48.0f;
        text->SetColor(1.0f, 1.0f, 1.0f, 1.0f);  // White
        text->SetFont("default");

        // Create player entity for input
        Entity player = world.GetEntityManager().CreateEntity();
        world.GetEntityManager().AddComponent<Playable>(player, Playable{ 0, MakeZeroInputBlob(), true });

        printf("[StartScreen] Renderer initialized!\n");
    }

    void Interpolate(const GameStateBlob& previousServerState,
        const GameStateBlob& currentServerState,
        const GameStateBlob& previousLocalState,
        const GameStateBlob& currentLocalState,
        GameStateBlob& renderState,
        float serverInterpolation,
        float localInterpolation) override {

        // For a simple start screen, just copy the current server state
        const StartScreenGameState& currServer = *reinterpret_cast<const StartScreenGameState*>(currentServerState.data);
        StartScreenGameState& rend = *reinterpret_cast<StartScreenGameState*>(renderState.data);

        rend = currServer;
    }

    ~StartScreenGameRenderer() override {
        // Cleanup if needed
    }

private:
    OpenGLWindow* window;
};

#endif