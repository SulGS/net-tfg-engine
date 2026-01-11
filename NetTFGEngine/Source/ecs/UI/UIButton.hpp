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
        , normalBorderColor(0.4f, 0.4f, 0.4f, 1.0f)
        , hoverBorderColor(0.6f, 0.6f, 0.6f, 1.0f)
        , pressedBorderColor(0.3f, 0.3f, 0.3f, 1.0f)
        , disabledBorderColor(0.3f, 0.3f, 0.3f, 0.5f)
        , textColor(0.0f, 0.0f, 0.0f, 1.0f)
        , fontName("default")
        , fontSize(16.0f)
        , padding(10.0f)
    {
    }

    ButtonState state;
    bool isInteractable;

    // Background colors for different states
    glm::vec4 normalColor;
    glm::vec4 hoverColor;
    glm::vec4 pressedColor;
    glm::vec4 disabledColor;

    // Border colors for different states
    glm::vec4 normalBorderColor;
    glm::vec4 hoverBorderColor;
    glm::vec4 pressedBorderColor;
    glm::vec4 disabledBorderColor;

    // Text properties
    std::string text;
    glm::vec4 textColor;
    std::string fontName;
    float fontSize;
    float padding;

    // Callbacks
    std::function<void()> onClick = nullptr;
    std::function<void()> onHoverEnter = nullptr;
    std::function<void()> onHoverExit = nullptr;

    // Get current background color based on state
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

    // Get current border color based on state
    glm::vec4 GetCurrentBorderColor() const {
        if (!isInteractable) return disabledBorderColor;

        switch (state) {
        case ButtonState::NORMAL:   return normalBorderColor;
        case ButtonState::HOVERED:  return hoverBorderColor;
        case ButtonState::PRESSED:  return pressedBorderColor;
        case ButtonState::DISABLED: return disabledBorderColor;
        default: return normalBorderColor;
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