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

#include "settings_menu.hpp"

// Id used to register this scene in main().
inline constexpr int MENU_SCENE_ID = 0;

// Player's form; survives the ECS rebuild when entering/leaving settings.
struct MenuFormMemory {
    std::string ip;
    std::string port;
    std::string clientName;
};

inline MenuFormMemory g_menuForm;

struct StartScreenGameState {
    bool spacePressed;
    int frameCount;
};

// Connect button state: pressed is an edge (consumed once), busy is level (lasts the whole attempt).
class ConnectButton : public IComponent {
public:
    bool pressed = false;
    bool busy = false;

    void Press() {
        pressed = true;
        busy = true;
    }

    // Devuelve true una sola vez por pulsacion.
    bool ConsumePress() {
        if (!pressed) return false;
        pressed = false;
        return true;
    }

    // Intento terminado en error: descarta la pulsacion pendiente, si la hay,
    // para que no se reintente sola.
    void Reset() {
        pressed = false;
        busy = false;
    }
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
    int remainTicks = CurrentTargetFPS() / 4;
    int currentState = 0;

    std::string errorMessage = "";
};

// Single place to write connection status text, instead of repeating the query in every branch.
inline void SetConnectStatus(EntityManager& em, bool connecting, const std::string& message) {
    auto query = em.CreateQuery<UIElement, UIText, TextAnimationData>();
    for (auto [entity, element, text, animData] : query) {
        animData->active = connecting;
        animData->errorMessage = message;
    }
}

// Only writer of the Connect button's isInteractable: clickable only while no connection attempt is in progress.
class ConnectButtonSystem : public ISystem {
public:
    void Update(EntityManager& entityManager, std::vector<EventEntry>& events, bool isServer, float deltaTime) override {
        auto query = entityManager.CreateQuery<UIButton, ConnectButton>();
        for (auto [entity, button, connectBtn] : query) {
            button->isInteractable = !connectBtn->busy;
        }
    }
};

// Animates text only — no longer touches UIButton (removed to avoid grabbing an arbitrary button).
class TextAnimationSystem : public ISystem {
    void Update(EntityManager& entityManager, std::vector<EventEntry>& events, bool isServer, float deltaTime) override {
        auto query = entityManager.CreateQuery<UIElement, UIText, TextAnimationData>();

        for (auto [entity, element, text, animData] : query) {
            if (!animData->active)
            {
                text->text = animData->errorMessage;
                continue;
            }

            animData->remainTicks--;
            if (animData->remainTicks <= 0) {
                animData->currentState = (animData->currentState + 1) % 4;
                animData->remainTicks = CurrentTargetFPS() / 4;
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
                            // Deferred (not DeactivateClient) since client 0 may still be mid-tick on this background thread.
                            NetTFG_Engine::Get().RequestDeactivateClient(MENU_SCENE_ID);
                        }
                        else {
                            Debug::Error("StartScreen") << "Client " << id << " failed: " << code << "\n";
                            conn->errorConnecting = true;
                            conn->connecting = false;
                            conn->goingToConnect = false;

                        }
                    }, conn->ip, stoi(conn->port), conn->clientName);
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
        InputBlob buf = MakeZeroInputBlob();
        return buf;
    }

    void GameState_To_ECSWorld(const GameStateBlob& state) override {
        StartScreenGameState s = *reinterpret_cast<const StartScreenGameState*>(state.data);

        // Nothing to sync from state into ECS for this simple screen.
    }

    void ECSWorld_To_GameState(GameStateBlob& state) override {
        StartScreenGameState& s = *reinterpret_cast<StartScreenGameState*>(state.data);

        s.frameCount++;

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

        s->spacePressed = false;
        s->frameCount = 0;
        state.len = sizeof(StartScreenGameState);

        world.GetEntityManager().RegisterComponentType<ConnectionData>();

        Entity player = world.GetEntityManager().CreateEntity();
        world.GetEntityManager().AddComponent<Playable>(player, Playable{ 0, MakeZeroInputBlob(), true });
        world.GetEntityManager().AddComponent<ConnectionData>(player, ConnectionData{ "", "", "" });

        world.AddSystem(std::make_unique<StartScreenInputSystem>());

        printf("[StartScreen] Game logic initialized!\n");
    }

    void HashState(const GameStateBlob& state, uint8_t(&outHash)[SHA256_DIGEST_LENGTH]) const override {
        EVP_MD_CTX* ctx = EVP_MD_CTX_new();
        if (!ctx) throw std::runtime_error("Failed to create EVP_MD_CTX");

        if (1 != EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr)) {
            EVP_MD_CTX_free(ctx);
            throw std::runtime_error("EVP_DigestInit_ex failed");
        }

        // No data to hash yet (would normally call EVP_DigestUpdate here).

        unsigned int len = 0;
        if (1 != EVP_DigestFinal_ex(ctx, outHash, &len)) {
            EVP_MD_CTX_free(ctx);
            throw std::runtime_error("EVP_DigestFinal_ex failed");
        }

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
    void GameState_To_ECSWorld(const GameStateBlob& state) override {
        StartScreenGameState s = *reinterpret_cast<const StartScreenGameState*>(state.data);

    }


    void InitECSRenderer(const GameStateBlob& state, OpenGLWindow* window) override {
        this->window = window;
        StartScreenGameState s;
        std::memcpy(&s, state.data, sizeof(StartScreenGameState));

        EntityManager& em = world.GetEntityManager();

        em.RegisterComponentType<TextAnimationData>();
        world.AddSystem(std::make_unique<TextAnimationSystem>());

        // Connect button state; must be registered before it's used below.
        em.RegisterComponentType<ConnectButton>();

        world.AddSystem(std::make_unique<ConnectButtonSystem>());

        Entity camera = em.CreateEntity();
        Transform* camTrans = em.AddComponent<Transform>(camera, Transform{});
        camTrans->setPosition(glm::vec3(0.0f, 0.0f, 2.0f));
        Camera* camSettings = em.AddComponent<Camera>(camera, Camera{});
        camSettings->setOrthographic(-100.0f, 100.0f, -100.0f, 100.0f, 0.1f, 100.0f);
        camSettings->setTarget(glm::vec3(0.0f, 0.0f, 0.0f));
        camSettings->setUp(glm::vec3(0.0f, 1.0f, 0.0f));

        // Create text field (LOWER layer = rendered first, behind other elements)
        Entity ipField = em.CreateEntity();
        UIElement* element = em.AddComponent<UIElement>(ipField);
        element->anchor = UIAnchor::TOP_LEFT;
        element->position = glm::vec2(100.0f, 10.0f);
        element->size = glm::vec2(300.0f, 40.0f);
        element->isVisible = true;
        element->layer = 1;

        UITextField* ipInput = em.AddComponent<UITextField>(ipField);
        ipInput->id = "ip_input";
        ipInput->placeholderText = "Enter IP here...";
        ipInput->fontSize = 16.0f;
        ipInput->padding = 10.0f;
        ipInput->maxLength = 100;
        ipInput->text = g_menuForm.ip;

        Entity portField = em.CreateEntity();
        element = em.AddComponent<UIElement>(portField);
        element->anchor = UIAnchor::TOP_LEFT;
        element->position = glm::vec2(100.0f, 55.0f);
        element->size = glm::vec2(300.0f, 40.0f);
        element->isVisible = true;
        element->layer = 1;

        UITextField* portInput = em.AddComponent<UITextField>(portField);
        portInput->id = "port_input";
        portInput->placeholderText = "Enter port here...";
        portInput->fontSize = 16.0f;
        portInput->padding = 10.0f;
        portInput->maxLength = 100;
        portInput->text = g_menuForm.port;

        Entity nameField = em.CreateEntity();
        element = em.AddComponent<UIElement>(nameField);
        element->anchor = UIAnchor::TOP_LEFT;
        element->position = glm::vec2(100.0f, 100.0f);
        element->size = glm::vec2(300.0f, 40.0f);
        element->isVisible = true;
        element->layer = 1;

        UITextField* nameInput = em.AddComponent<UITextField>(nameField);
        nameInput->id = "name_input";
        nameInput->placeholderText = "Enter name here...";
        nameInput->fontSize = 16.0f;
        nameInput->padding = 10.0f;
        nameInput->maxLength = 100;
        nameInput->text = g_menuForm.clientName;

        Entity buttonElement = em.CreateEntity();
        element = em.AddComponent<UIElement>(buttonElement, UIElement{});
        element->anchor = UIAnchor::TOP_LEFT;
        element->position = glm::vec2(100.0f, 160.0f);
        element->size = glm::vec2(300.0f, 40.0f);
        element->layer = 10;  // higher layer number renders on top

        UIButton* button = em.AddComponent<UIButton>(buttonElement);
        button->text = "Connect";

        // onClick only records the press; ConnectButtonSystem disables the button once busy == true.
        ConnectButton* connectBtn = em.AddComponent<ConnectButton>(buttonElement);
        button->onClick = [connectBtn]() {
            connectBtn->Press();
            };

        Entity startText = em.CreateEntity();
        element = em.AddComponent<UIElement>(startText, UIElement{});
        element->anchor = UIAnchor::CENTER;
        element->position = glm::vec2(0.0f, 0.0f);
        element->size = glm::vec2(400.0f, 80.0f);
        element->pivot = glm::vec2(0.5f, 0.5f);
        element->layer = 10;

        UIText* text = em.AddComponent<UIText>(startText, UIText{});
        text->text = "";
        text->fontSize = 18.0f;
        text->SetColor(1.0f, 1.0f, 1.0f, 1.0f);
        text->SetFont("default");

        Entity imageEntity = em.CreateEntity();
        UIElement* imgElement = em.AddComponent<UIElement>(imageEntity, UIElement{});
        imgElement->anchor = UIAnchor::BOTTOM_RIGHT;
        imgElement->position = glm::vec2(-400.0f, -400.0f);
        imgElement->size = glm::vec2(400.0f, 400.0f);
        imgElement->layer = 5;
        UIImage* imgComp = em.AddComponent<UIImage>(imageEntity, UIImage{});
        imgComp->texturePath = "spaceboard.png";


        em.AddComponent<TextAnimationData>(startText);

        // Settings entry button; just requests the scene change.
        Entity settingsButton = em.CreateEntity();
        element = em.AddComponent<UIElement>(settingsButton, UIElement{});
        element->anchor = UIAnchor::TOP_LEFT;
        element->position = glm::vec2(100.0f, 210.0f);
        element->size = glm::vec2(300.0f, 40.0f);
        element->layer = 10;

        UIButton* settingsBtn = em.AddComponent<UIButton>(settingsButton);
        settingsBtn->text = "Ajustes";
        settingsBtn->onClick = [ipInput, portInput, nameInput]() {
            // Guarda el formulario antes de que este mundo se destruya.
            g_menuForm.ip = ipInput->text;
            g_menuForm.port = portInput->text;
            g_menuForm.clientName = nameInput->text;
            OpenSettingsFrom(MENU_SCENE_ID);
            };

		Entity exitButton = em.CreateEntity();
		element = em.AddComponent<UIElement>(exitButton, UIElement{});
		element->anchor = UIAnchor::TOP_LEFT;
		element->position = glm::vec2(100.0f, 260.0f);
		element->size = glm::vec2(300.0f, 40.0f);
		element->layer = 10;

		UIButton* exitBtn = em.AddComponent<UIButton>(exitButton);
		exitBtn->text = "Salir";
		exitBtn->onClick = []() {
			ClientWindow::GetWindow()->close();
			};

        Entity player = em.CreateEntity();
        em.AddComponent<Playable>(player, Playable{ 0, MakeZeroInputBlob(), true });

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
            auto& em = gameRenderer->world.GetEntityManager();

            // No longer need to check if the settings panel is open: this is the button's own data now.
            auto connQuery = em2.CreateQuery<ConnectionData>();

            for (auto [connEntity, connData] : connQuery) {

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

                // Query keys off the button's state component, not just "any UIButton".
                auto buttonQuery = em.CreateQuery<ConnectButton>();

                for (auto [buttonEntity, connectBtn] : buttonQuery) {

                    if (connData->errorConnecting)
                    {
                        connData->errorConnecting = false;
                        connectBtn->Reset();
                        SetConnectStatus(em, false,
                            "Error connecting to " + connData->ip + ":" + connData->port);
                    }
                    else if (connectBtn->ConsumePress()) {
                        connData->goingToConnect = true;
                        SetConnectStatus(em, true, "");
                    }
                }
            }



            };


        AudioManager::SetMusicVolume(0.25f);
        AudioManager::PlayMusic("BandaSonora.wav", true);
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
    }

private:
    OpenGLWindow* window;
};

#endif