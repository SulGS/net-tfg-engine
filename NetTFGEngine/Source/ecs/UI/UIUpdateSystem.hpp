#ifndef UIUPDATESYSTEM_HPP
#define UIUPDATESYSTEM_HPP

#include "ecs/ecs_common.hpp"
#include "UIElement.hpp"
#include "UITextField.hpp"
#include "UIButton.hpp"
#include "UISlider.hpp"
#include "UIDropdown.hpp"
#include "Utils/FontManager.hpp"
#include "OpenGL/OpenGLIncludes.hpp"

class UIUpdateSystem : public ISystem {
public:
    UIUpdateSystem(int refWidth, int refHeight, GLFWwindow* window, FontManager* fontMgr);
    ~UIUpdateSystem();

    void Update(EntityManager& entityManager, std::vector<EventEntry>& events, bool isServer, float deltaTime) override;
    void UpdateScreenSize(int width, int height);

    // Input handling
    void OnMouseMove(float x, float y);
    void OnMouseDown(float x, float y);
    void OnMouseUp(float x, float y);

    // The wheel is read from Input::GetScrollDelta() inside Update(). This is
    // only needed if you want to feed it from somewhere else.
    // Returns true when the scroll was consumed by the UI.
    bool OnScroll(float yOffset);

    // Focus management
    void SetFocus(EntityManager& entityManager, Entity entity);
    void ClearFocus(EntityManager& entityManager);
    Entity GetFocusedEntity() const { return focusedTextField; }

    // Slider / dropdown state
    Entity GetActiveSlider() const { return activeSlider; }
    Entity GetOpenDropdown() const { return openDropdown; }
    bool IsInteractingWithUI() const {
        return focusedTextField != 0 || activeSlider != 0 || openDropdown != 0;
    }
    void CloseAllDropdowns(EntityManager& entityManager);

    // Initialize GLFW callbacks
    void SetupCallbacks();

private:
    int refWidth;
    int refHeight;
    int screenWidth;
    int screenHeight;
    glm::vec2 mousePosition;
    bool mouseDown;

    GLFWwindow* window;
    FontManager* fontManager;
    EntityManager* cachedEntityManager;  // Cache for callbacks

    Entity focusedTextField;
    float cursorBlinkInterval;

    // Slider state
    Entity activeSlider;      // slider currently being dragged
    Entity focusedSlider;     // last clicked slider (keyboard arrows)
    // Dropdown state
    Entity openDropdown;      // only one popup can be open at a time
    bool prevMouseDown;       // previous button state, used for the press edge

    // Static callback wrappers (GLFW requires static functions)
    static void CharCallback(GLFWwindow* window, unsigned int codepoint);
    static void KeyCallback(GLFWwindow* window, int key, int scancode, int action, int mods);

    // Instance handlers
    void HandleCharInput(unsigned int codepoint);
    void HandleKeyInput(int key, int scancode, int action, int mods);

    // Helper methods
    void UpdateTextField(EntityManager& entityManager, Entity entity, UIElement* element,
        UITextField* textField, float deltaTime);
    void HandleTextFieldClick(EntityManager& entityManager,
        Entity entity,
        UIElement* element,
        UITextField* textField,
        const glm::vec2& mousePos);

    size_t GetCursorPositionFromMouse(const UITextField* textField, const UIElement* element,
        const glm::vec2& localMousePos);

    bool IsMouseLeftDown() const;

    // Returns true when the click was consumed by a slider or a dropdown
    bool HandleDropdownClick(EntityManager& entityManager, const glm::vec2& refMouse);
    bool HandleSliderClick(EntityManager& entityManager, const glm::vec2& refMouse);

    void UpdateSliders(EntityManager& entityManager, const glm::vec2& refMouse,
        bool mouseIsDown, float deltaTime);
    void UpdateDropdowns(EntityManager& entityManager, const glm::vec2& refMouse);
    void HandleSliderKeyboard(EntityManager& entityManager);
    void HandleDropdownKeyboard(EntityManager& entityManager);
    void EndSliderDrag(EntityManager& entityManager);
};

#endif // UIUPDATESYSTEM_HPP