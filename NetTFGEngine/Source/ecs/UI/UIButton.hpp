#ifndef UIBUTTON_HPP
#define UIBUTTON_HPP

#include "ecs/ecs_common.hpp"
#include <functional>
#include "OpenGL/OpenGLIncludes.hpp"

enum class ButtonState {
    NORMAL,
    HOVERED,
    PRESSED,
    DISABLED
};

class UIButton : public IComponent {
public:
    UIButton()
        : state(ButtonState::NORMAL)
        , isInteractable(true)
        , normalColor(0.8f, 0.8f, 0.8f, 1.0f)
        , hoverColor(1.0f, 1.0f, 1.0f, 1.0f)
        , pressedColor(0.6f, 0.6, 0.6f, 1.0f)
        , disabledColor(0.5f, 0.5f, 0.5f, 0.5f)
    {}

    ButtonState state;
    bool isInteractable;
    
    // Colors for different states
    glm::vec4 normalColor;
    glm::vec4 hoverColor;
    glm::vec4 pressedColor;
    glm::vec4 disabledColor;
    
    // Callbacks
    std::function<void()> onClick = nullptr;
    std::function<void()> onHoverEnter = nullptr;
    std::function<void()> onHoverExit = nullptr;
    
    // Get current color based on state
    glm::vec4 GetCurrentColor() const {
        if (!isInteractable) return disabledColor;
        
        switch (state) {
            case ButtonState::NORMAL:   return normalColor;
            case ButtonState::HOVERED:  return hoverColor;
            case ButtonState::PRESSED:  return pressedColor;
            case ButtonState::DISABLED: return disabledColor;
            default: return normalColor;
        }
    }
    
    // Chainable setters
    UIButton* SetOnClick(std::function<void()> callback) {
        onClick = callback;
        return this;
    }
    
    UIButton* SetOnHoverEnter(std::function<void()> callback) {
        onHoverEnter = callback;
        return this;
    }
    
    UIButton* SetOnHoverExit(std::function<void()> callback) {
        onHoverExit = callback;
        return this;
    }
};

#endif // UIBUTTON_HPP