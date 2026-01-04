#ifndef UIUPDATESYSTEM_HPP
#define UIUPDATESYSTEM_HPP

#include "ecs/ecs_common.hpp"
#include "UIElement.hpp"
#include "UITextField.hpp"
#include "Utils/FontManager.hpp"
#include "OpenGL/OpenGLIncludes.hpp"

class UIUpdateSystem : public ISystem {
public:
    UIUpdateSystem(int screenWidth, int screenHeight, GLFWwindow* window, FontManager* fontMgr);
    ~UIUpdateSystem();

    void Update(EntityManager& entityManager, std::vector<EventEntry>& events, bool isServer, float deltaTime) override;
    void UpdateScreenSize(int width, int height);

    // Input handling
    void OnMouseMove(float x, float y);
    void OnMouseDown(float x, float y);
    void OnMouseUp(float x, float y);

    // Focus management
    void SetFocus(EntityManager& entityManager, Entity entity);
    void ClearFocus(EntityManager& entityManager);
    Entity GetFocusedEntity() const { return focusedTextField; }

    // Initialize GLFW callbacks
    void SetupCallbacks();

private:
    int screenWidth;
    int screenHeight;
    glm::vec2 mousePosition;
    bool mouseDown;

    GLFWwindow* window;
    FontManager* fontManager;
    EntityManager* cachedEntityManager;  // Cache for callbacks

    Entity focusedTextField;
    float cursorBlinkInterval;

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
};

#endif // UIUPDATESYSTEM_HPP