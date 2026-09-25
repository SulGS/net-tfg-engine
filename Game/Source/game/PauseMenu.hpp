#pragma once

#include "ecs/ecs.hpp"
#include "ecs/UI/UIButton.hpp"
#include "ecs/UI/UIElement.hpp"
#include "ecs/UI/UIImage.hpp"
#include "ecs/UI/UIText.hpp"
#include "Utils/Input.hpp"
#include "Components.hpp"

// In-match "exit?" menu, toggled with Escape. Render world only: the match keeps running
// underneath (it's online), the menu just offers going back to the main menu.

// State of the menu; one instance, on the panel entity.
class PauseMenuState : public IComponent {
public:
    bool visible = false;
};

// Tags every entity of the menu, so PauseMenuSystem shows/hides them together and the
// game's own button logic (CameraFollowSystem, OnDeathRenderSystem) leaves them alone.
class PauseMenuItem : public IComponent {
};

// Only writer of the menu's visibility.
class PauseMenuSystem : public ISystem {
public:
    void Update(EntityManager& entityManager, std::vector<EventEntry>& events, bool isServer, float deltaTime) override {
        auto stateQuery = entityManager.CreateQuery<PauseMenuState>();
        for (auto [entity, state] : stateQuery) {
            if (Input::KeyTapped(Input::Escape)) {
                state->visible = !state->visible;
            }

            auto itemQuery = entityManager.CreateQuery<UIElement, PauseMenuItem>();
            for (auto [itemEntity, element, item] : itemQuery) {
                element->isVisible = state->visible;
            }
        }
    }
};

inline void RegisterPauseMenuComponents(EntityManager& em) {
    em.RegisterComponentType<PauseMenuState>();
    em.RegisterComponentType<PauseMenuItem>();
}

// "Sí" raises exitChecker->exitPressed, the same path as the game-over Exit button, so
// ExitCheckerSystem takes the player back to the main menu.
inline void BuildPauseMenu(EntityManager& em, ExitButtonChecker* exitChecker) {
    constexpr int LAYER = 50;  // above the HUD and the game-over Exit button

    Entity panel = em.CreateEntity();
    UIElement* element = em.AddComponent<UIElement>(panel, UIElement{});
    element->anchor = UIAnchor::CENTER;
    element->position = glm::vec2(0.0f, 0.0f);
    element->size = glm::vec2(420.0f, 180.0f);
    element->pivot = glm::vec2(0.5f, 0.5f);
    element->isVisible = false;
    element->layer = LAYER;
    UIImage* image = em.AddComponent<UIImage>(panel, UIImage{});
    image->texturePath = "ui_panel.png";
    em.AddComponent<PauseMenuItem>(panel);
    PauseMenuState* state = em.AddComponent<PauseMenuState>(panel, PauseMenuState{});

    Entity question = em.CreateEntity();
    element = em.AddComponent<UIElement>(question, UIElement{});
    element->anchor = UIAnchor::CENTER;
    element->position = glm::vec2(0.0f, -35.0f);
    element->size = glm::vec2(380.0f, 40.0f);
    element->pivot = glm::vec2(0.5f, 0.5f);
    element->isVisible = false;
    element->layer = LAYER + 1;
    UIText* text = em.AddComponent<UIText>(question, UIText{});
    text->text = "¿Estás seguro de querer salir?";
    text->fontSize = 24.0f;
    text->align = UITextAlign::CENTER;
    text->SetColor(1.0f, 1.0f, 1.0f, 1.0f);
    text->SetFont("default");
    em.AddComponent<PauseMenuItem>(question);

    Entity yesButton = em.CreateEntity();
    element = em.AddComponent<UIElement>(yesButton, UIElement{});
    element->anchor = UIAnchor::CENTER;
    element->position = glm::vec2(-80.0f, 35.0f);
    element->size = glm::vec2(140.0f, 45.0f);
    element->pivot = glm::vec2(0.5f, 0.5f);
    element->isVisible = false;
    element->layer = LAYER + 1;
    UIButton* yes = em.AddComponent<UIButton>(yesButton, UIButton{});
    yes->text = "Sí";
    yes->fontSize = 24.0f;
    yes->onClick = [yes, exitChecker]() {
        Debug::Info("Asteroids") << "Pause menu: exiting to main menu.\n";
        yes->isInteractable = false;  // prevent multiple clicks
        exitChecker->exitPressed = true;
    };
    em.AddComponent<PauseMenuItem>(yesButton);

    Entity noButton = em.CreateEntity();
    element = em.AddComponent<UIElement>(noButton, UIElement{});
    element->anchor = UIAnchor::CENTER;
    element->position = glm::vec2(80.0f, 35.0f);
    element->size = glm::vec2(140.0f, 45.0f);
    element->pivot = glm::vec2(0.5f, 0.5f);
    element->isVisible = false;
    element->layer = LAYER + 1;
    UIButton* no = em.AddComponent<UIButton>(noButton, UIButton{});
    no->text = "No";
    no->fontSize = 24.0f;
    no->onClick = [state]() {
        state->visible = false;
    };
    em.AddComponent<PauseMenuItem>(noButton);
}
