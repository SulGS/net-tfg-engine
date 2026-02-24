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

#include <openssl/evp.h>

// Simple game state for start screen
struct StartScreenGameState {
    bool spacePressed;
    int frameCount;
};

class ConnectionData : public IComponent {
public:
    std::string ip;
    std::string port;
    std::string clientName;
	bool goingToConnect = false;
	bool connecting = false;
	bool errorConnecting = false;

	ConnectionData() : ip(""), port(""), clientName(""), goingToConnect(false), connecting(false) {}
	ConnectionData(const std::string& ip, const std::string& port, const std::string& clientName)
		: ip(ip), port(port), clientName(clientName), goingToConnect(false), connecting(false) {
	}
};

class TextAnimationData : public IComponent {
public:
	bool active = false;
    int remainTicks = RENDER_TICKS_PER_SECOND / 4;
    int currentState = 0;

	std::string errorMessage = "";
};

class TextAnimationSystem : public ISystem {
	void Update(EntityManager& entityManager, std::vector<EventEntry>& events, bool isServer, float deltaTime) override {
		auto query = entityManager.CreateQuery<UIElement, UIText, TextAnimationData>();

        auto buttonQuery = entityManager.CreateQuery<UIButton>();
		UIButton* button = nullptr;

		for (auto [buttonEntity, btn] : buttonQuery) {
			button = btn;
		}

		for (auto [entity, element, text, animData] : query) {
            if (!animData->active) 
            {
                text->text = animData->errorMessage;
				button->isInteractable = true;
				continue;
            }

			if(button) button->isInteractable = false;

			animData->remainTicks--;
			if (animData->remainTicks <= 0) {
				animData->currentState = (animData->currentState + 1) % 4;
				animData->remainTicks = RENDER_TICKS_PER_SECOND / 4;
				switch (animData->currentState) {
				case 0:
					text->text = "Conectando.";
					break;
				case 1:
                    text->text = "Conectando..";
					break;
				case 2:
                    text->text = "Conectando...";
					break;
                case 3:
                    text->text = "Conectando";
					break;
				}
			}
		}
	}
};

// System to detect space key press
class StartScreenInputSystem : public ISystem {
public:
    void Update(EntityManager& entityManager, std::vector<EventEntry>& events, bool isServer, float deltaTime) override {
        auto query = entityManager.CreateQuery<ConnectionData>();

        for (auto [entity, conn] : query) {

            if (conn->goingToConnect && !conn->connecting) {
				Debug::Info("StartScreen") << "Connecting to server at " << conn->ip << ":" << conn->port << "\n";
                NetTFG_Engine::Get().ActivateClientAsync(1,
                    [conn](int id, ConnectionCode code) {
                        if (code == CONN_SUCCESS) {
                            Debug::Info("StartScreen") << "Client " << id << " activated!\n";
							NetTFG_Engine::Get().DeactivateClient(0);
                        }
                        else {
                            Debug::Error("StartScreen") << "Client " << id << " failed: " << code << "\n";
							conn->errorConnecting = true;
							conn->connecting = false;
							conn->goingToConnect = false;
							
                        }
                    }, conn->ip,stoi(conn->port),conn->clientName);
				conn->connecting = true;
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
        //uint8_t m = INPUT_NULL;
        InputBlob buf = MakeZeroInputBlob();

        /*if (Input::IsInputBlockedForUI()) return buf;

        if (Input::KeyPressed(Input::CharToKeycode(' '))) {
            m |= INPUT_SPACE;
        }

        
        buf.data[0] = m;*/
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
        auto query = world.GetEntityManager().CreateQuery<ConnectionData>();
        for (auto [entity, connData] : query) {

            if (connData->goingToConnect) {
                s.spacePressed = true;
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

		world.GetEntityManager().RegisterComponentType<ConnectionData>();

        // Create a player entity to receive input
        Entity player = world.GetEntityManager().CreateEntity();
        world.GetEntityManager().AddComponent<Playable>(player, Playable{ 0, MakeZeroInputBlob(), true });
        world.GetEntityManager().AddComponent<ConnectionData>(player, ConnectionData{ "", "", "" });

        // Add input detection system
        world.AddSystem(std::make_unique<StartScreenInputSystem>());

        printf("[StartScreen] Game logic initialized!\n");
    }

    void HashState(const GameStateBlob& state, uint8_t(&outHash)[SHA256_DIGEST_LENGTH]) const override {
        // Create context
        EVP_MD_CTX* ctx = EVP_MD_CTX_new();
        if (!ctx) throw std::runtime_error("Failed to create EVP_MD_CTX");

        // Initialize SHA-256
        if (1 != EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr)) {
            EVP_MD_CTX_free(ctx);
            throw std::runtime_error("EVP_DigestInit_ex failed");
        }

        // No data to hash currently
        // (would normally call EVP_DigestUpdate(ctx, data, size); here)

        // Finalize hash
        unsigned int len = 0;
        if (1 != EVP_DigestFinal_ex(ctx, outHash, &len)) {
            EVP_MD_CTX_free(ctx);
            throw std::runtime_error("EVP_DigestFinal_ex failed");
        }

        // Cleanup
        EVP_MD_CTX_free(ctx);
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
        /*auto textQuery = world.GetEntityManager().CreateQuery<UIElement, UIText>();
        for (auto [entity, element, text] : textQuery) {
            if (s.spacePressed) {
                text->text = "Starting...";
            }
        }*/
    }


    void InitECSRenderer(const GameStateBlob& state, OpenGLWindow* window) override {
        this->window = window;
        StartScreenGameState s;
        std::memcpy(&s, state.data, sizeof(StartScreenGameState));

		world.GetEntityManager().RegisterComponentType<TextAnimationData>();
		world.AddSystem(std::make_unique<TextAnimationSystem>());

        // Create camera
        Entity camera = world.GetEntityManager().CreateEntity();
        Transform* camTrans = world.GetEntityManager().AddComponent<Transform>(camera, Transform{});
        camTrans->setPosition(glm::vec3(0.0f, 0.0f, 2.0f));
        Camera* camSettings = world.GetEntityManager().AddComponent<Camera>(camera, Camera{});
        camSettings->setOrthographic(-100.0f, 100.0f, -100.0f, 100.0f, 0.1f, 100.0f);
        camSettings->setTarget(glm::vec3(0.0f, 0.0f, 0.0f));
        camSettings->setUp(glm::vec3(0.0f, 1.0f, 0.0f));

        // Create text field (LOWER layer = rendered first, behind other elements)
        Entity ipField = world.GetEntityManager().CreateEntity();
        UIElement* element = world.GetEntityManager().AddComponent<UIElement>(ipField);
        element->anchor = UIAnchor::TOP_LEFT;
        element->position = glm::vec2(100.0f, 10.0f);
        element->size = glm::vec2(300.0f, 40.0f);
        element->isVisible = true;
        element->layer = 1;  // Lower layer number

        UITextField* field = world.GetEntityManager().AddComponent<UITextField>(ipField);
		field->id = "ip_input";
        field->placeholderText = "Enter IP here...";
        field->fontSize = 16.0f;
        field->padding = 10.0f;
        field->maxLength = 100;

        Entity portField = world.GetEntityManager().CreateEntity();
        element = world.GetEntityManager().AddComponent<UIElement>(portField);
        element->anchor = UIAnchor::TOP_LEFT;
        element->position = glm::vec2(100.0f, 55.0f);
        element->size = glm::vec2(300.0f, 40.0f);
        element->isVisible = true;
        element->layer = 1;  // Lower layer number

        field = world.GetEntityManager().AddComponent<UITextField>(portField);
		field->id = "port_input";
        field->placeholderText = "Enter port here...";
        field->fontSize = 16.0f;
        field->padding = 10.0f;
        field->maxLength = 100;

        Entity nameField = world.GetEntityManager().CreateEntity();
        element = world.GetEntityManager().AddComponent<UIElement>(nameField);
        element->anchor = UIAnchor::TOP_LEFT;
        element->position = glm::vec2(100.0f, 100.0f);
        element->size = glm::vec2(300.0f, 40.0f);
        element->isVisible = true;
        element->layer = 1;  // Lower layer number

        field = world.GetEntityManager().AddComponent<UITextField>(nameField);
		field->id = "name_input";
        field->placeholderText = "Enter name here...";
        field->fontSize = 16.0f;
        field->padding = 10.0f;
        field->maxLength = 100;

        Entity buttonElement = world.GetEntityManager().CreateEntity();
        element = world.GetEntityManager().AddComponent<UIElement>(buttonElement, UIElement{});
        element->anchor = UIAnchor::TOP_LEFT;
        element->position = glm::vec2(100.0f, 160.0f);
        element->size = glm::vec2(300.0f, 40.0f);
        element->layer = 10;  // HIGHER layer number - renders on top!

        UIButton* button = world.GetEntityManager().AddComponent<UIButton>(buttonElement);
		button->text = "Connect";

		button->onClick = [this, button]() {
			button->isInteractable = false; // Disable button after click
		};

        // Create UI text entity (HIGHER layer = rendered last, on top of everything)
        Entity startText = world.GetEntityManager().CreateEntity();
        element = world.GetEntityManager().AddComponent<UIElement>(startText, UIElement{});
        element->anchor = UIAnchor::CENTER;
        element->position = glm::vec2(0.0f, 0.0f);
        element->size = glm::vec2(400.0f, 80.0f);
        element->pivot = glm::vec2(0.5f, 0.5f);
        element->layer = 10;  // HIGHER layer number - renders on top!

        UIText* text = world.GetEntityManager().AddComponent<UIText>(startText, UIText{});
        text->text = "";
        text->fontSize = 18.0f;
        text->SetColor(1.0f, 1.0f, 1.0f, 1.0f);  // White
        text->SetFont("default");

		Entity imageEntity = world.GetEntityManager().CreateEntity();
		UIElement* imgElement = world.GetEntityManager().AddComponent<UIElement>(imageEntity, UIElement{});
		imgElement->anchor = UIAnchor::BOTTOM_RIGHT;
		imgElement->position = glm::vec2(-400.0f, -400.0f);
		imgElement->size = glm::vec2(400.0f, 400.0f);
		imgElement->layer = 5;
		UIImage* imgComp = world.GetEntityManager().AddComponent<UIImage>(imageEntity, UIImage{});
		imgComp->texturePath = "spaceboard.png";
        

		world.GetEntityManager().AddComponent<TextAnimationData>(startText);

        // Create player entity for input
        Entity player = world.GetEntityManager().CreateEntity();
        world.GetEntityManager().AddComponent<Playable>(player, Playable{ 0, MakeZeroInputBlob(), true });

        renderDataTransferToLogicCallback = [](IECSGameLogic* logic, IECSGameRenderer* renderer) {
            if (!logic) {
                return;
            }

			StartScreenGame* gameLogic = dynamic_cast<StartScreenGame*>(logic);
			StartScreenGameRenderer* gameRenderer = dynamic_cast<StartScreenGameRenderer*>(renderer);
			if (!gameLogic || !gameRenderer) {
				return;
			}

            auto& em2 = gameLogic->world.GetEntityManager();
            auto connQuery = em2.CreateQuery<ConnectionData>();

            for (auto [connEntity, connData] : connQuery) {

                auto& em = gameRenderer->world.GetEntityManager();
                auto query = em.CreateQuery<UIElement, UITextField>();

                for (auto [entity, element, textField] : query) {
                    if (textField->id == "ip_input") {
                        connData->ip = textField->text;
                    }
                    else if (textField->id == "port_input") {
                        connData->port = textField->text;
                    }
                    else if (textField->id == "name_input") {
                        connData->clientName = textField->text;
                    }
                }

				auto buttonQuery = em.CreateQuery<UIButton>();

                for (auto [buttonEntity, button] : buttonQuery) {

                    if (connData->errorConnecting) 
                    {
						connData->errorConnecting = false;
						button->isInteractable = true;
						auto textAnimQuery = em.CreateQuery<UIElement, UIText, TextAnimationData>();
						for (auto [taEntity, taElement, taText, taData] : textAnimQuery) {
							taData->active = false;
							taData->errorMessage = "Error connecting to " + connData->ip + ":" + connData->port;
						}
                    }
                    else if (!button->isInteractable) {
                        connData->goingToConnect = true;
						auto textAnimQuery = em.CreateQuery<UIElement, UIText, TextAnimationData>();
                        for (auto [taEntity, taElement, taText, taData] : textAnimQuery) {
                            taData->active = true;
                        }
                    }
                }
            }

			
			
        };


        AudioManager::SetMusicVolume(0.25f);
		AudioManager::PlayMusic("BandaSonora.wav", true);
        //printf("[StartScreen] Renderer initialized!\n");
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