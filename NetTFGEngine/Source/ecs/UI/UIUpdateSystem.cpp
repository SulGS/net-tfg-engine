#include "UIUpdateSystem.hpp"
#include "Utils/Input.hpp"
#include <algorithm>
#include <iostream>

// Static pointer for callback access
static UIUpdateSystem* g_UIUpdateSystemInstance = nullptr;

UIUpdateSystem::UIUpdateSystem(int width, int height, GLFWwindow* win, FontManager* fontMgr)
    : screenWidth(width)
    , screenHeight(height)
    , mousePosition(0.0f)
    , mouseDown(false)
    , window(win)
    , fontManager(fontMgr)
    , cachedEntityManager(nullptr)
    , focusedTextField(0)
    , cursorBlinkInterval(0.53f)
{
    g_UIUpdateSystemInstance = this;
    SetupCallbacks();
}

UIUpdateSystem::~UIUpdateSystem() {
    if (g_UIUpdateSystemInstance == this) {
        g_UIUpdateSystemInstance = nullptr;
    }
}

void UIUpdateSystem::SetupCallbacks() {
    if (!window) return;

    // Set character callback for text input
    glfwSetCharCallback(window, CharCallback);

    // Note: Key callback is already set by Input class, so we'll handle
    // special keys in the Update method using Input::KeyTapped
}

void UIUpdateSystem::CharCallback(GLFWwindow* window, unsigned int codepoint) {
    if (g_UIUpdateSystemInstance) {
        g_UIUpdateSystemInstance->HandleCharInput(codepoint);
    }
}

void UIUpdateSystem::KeyCallback(GLFWwindow* window, int key, int scancode, int action, int mods) {
    if (g_UIUpdateSystemInstance) {
        g_UIUpdateSystemInstance->HandleKeyInput(key, scancode, action, mods);
    }
}

void UIUpdateSystem::HandleCharInput(unsigned int codepoint) {
    if (focusedTextField == 0 || !cachedEntityManager) return;

    auto textField = cachedEntityManager->GetComponent<UITextField>(focusedTextField);
    if (!textField || !textField->isInteractable || textField->state != TextFieldState::FOCUSED) {
        return;
    }

    // Convert Unicode codepoint to UTF-8 string
    std::string text;
    if (codepoint < 0x80) {
        text += static_cast<char>(codepoint);
    }
    else if (codepoint < 0x800) {
        text += static_cast<char>(0xC0 | (codepoint >> 6));
        text += static_cast<char>(0x80 | (codepoint & 0x3F));
    }
    else if (codepoint < 0x10000) {
        text += static_cast<char>(0xE0 | (codepoint >> 12));
        text += static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F));
        text += static_cast<char>(0x80 | (codepoint & 0x3F));
    }
    else {
        text += static_cast<char>(0xF0 | (codepoint >> 18));
        text += static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F));
        text += static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F));
        text += static_cast<char>(0x80 | (codepoint & 0x3F));
    }

    // Validate before inserting
    if (textField->validator) {
        std::string testText = textField->text;
        if (textField->HasSelection()) {
            size_t start = textField->GetSelectionMin();
            size_t end = textField->GetSelectionMax();
            testText.erase(start, end - start);
            testText.insert(start, text);
        }
        else {
            testText.insert(textField->cursorPosition, text);
        }

        if (!textField->validator(testText)) {
            return;  // Invalid input
        }
    }

    textField->InsertText(text);
    textField->cursorBlinkTime = 0.0f;
    textField->cursorVisible = true;
}

void UIUpdateSystem::HandleKeyInput(int key, int scancode, int action, int mods) {
    // This is called by GLFW key callback if needed
    // For now we handle keys in Update() using Input class
}

// Replace the UpdateTextField and Update methods in UIUpdateSystem.cpp

void UIUpdateSystem::Update(EntityManager& entityManager, std::vector<EventEntry>& events,
    bool isServer, float deltaTime) {
    cachedEntityManager = &entityManager;

    double mouseX, mouseY;
    Input::GetMousePosition(mouseX, mouseY);

    if (Input::MousePressed(GLFW_MOUSE_BUTTON_LEFT)) {
        bool clickedAnyField = false;
        Entity clickedField = 0;
        bool clickedButton = false;

        // Check buttons first
        auto buttonQuery = entityManager.CreateQuery<UIElement, UIButton>();
        for (auto [entity, element, button] : buttonQuery) {
            if (!element->isVisible || !button->isInteractable) continue;
            if (element->Contains({ mouseX, mouseY }, screenWidth, screenHeight)) {
                if (button->onClick) button->onClick();
                clickedButton = true;
                break;
            }
        }

        // Check text fields only if no button was clicked
        if (!clickedButton) {
            auto query = entityManager.CreateQuery<UIElement, UITextField>();
            for (auto [entity, element, textField] : query) {
                if (!element->isVisible || !textField->isInteractable) continue;
                if (element->Contains({ mouseX, mouseY }, screenWidth, screenHeight)) {
                    clickedAnyField = true;
                    clickedField = entity;
                    break;
                }
            }

            if (clickedAnyField) {
                if (focusedTextField != clickedField) {
                    SetFocus(entityManager, clickedField);
                }
                auto element = entityManager.GetComponent<UIElement>(clickedField);
                auto textField = entityManager.GetComponent<UITextField>(clickedField);
                if (element && textField) {
                    HandleTextFieldClick(entityManager, clickedField, element, textField,
                        glm::vec2(mouseX, mouseY));
                }
            }
            else {
                if (focusedTextField != 0) {
                    ClearFocus(entityManager);
                }
            }
        }
        else {
            // Button was clicked, clear text field focus
            if (focusedTextField != 0) {
                ClearFocus(entityManager);
            }
        }
    }

    // Update button hover states every frame
    auto buttonQuery = entityManager.CreateQuery<UIElement, UIButton>();
    for (auto [entity, element, button] : buttonQuery) {
        if (!element->isVisible || !button->isInteractable) {
            button->state = ButtonState::DISABLED;
            continue;
        }

        bool isInside = element->Contains({ mouseX, mouseY }, screenWidth, screenHeight);
        ButtonState previousState = button->state;

        if (Input::KeyPressed(GLFW_MOUSE_BUTTON_LEFT) && isInside) {
            button->state = ButtonState::PRESSED;
        }
        else if (isInside) {
            if (previousState != ButtonState::HOVERED) {
                if (button->onHoverEnter) button->onHoverEnter();
            }
            button->state = ButtonState::HOVERED;
        }
        else {
            if (previousState == ButtonState::HOVERED) {
                if (button->onHoverExit) button->onHoverExit();
            }
            button->state = ButtonState::NORMAL;
        }
    }

    // Handle keyboard input for focused text field
    if (focusedTextField != 0) {
        auto textField = entityManager.GetComponent<UITextField>(focusedTextField);
        if (textField && textField->isInteractable && textField->state == TextFieldState::FOCUSED) {
            bool shift = Input::KeyPressed(GLFW_KEY_LEFT_SHIFT) || Input::KeyPressed(GLFW_KEY_RIGHT_SHIFT);
            bool ctrl = Input::KeyPressed(GLFW_KEY_LEFT_CONTROL) || Input::KeyPressed(GLFW_KEY_RIGHT_CONTROL);
            bool alt = Input::KeyPressed(GLFW_KEY_LEFT_ALT) || Input::KeyPressed(GLFW_KEY_RIGHT_ALT);

            if (Input::KeyTapped(GLFW_KEY_BACKSPACE)) {
                textField->Backspace();
                textField->cursorBlinkTime = 0.0f;
                textField->cursorVisible = true;
            }

            if (Input::KeyTapped(GLFW_KEY_DELETE)) {
                textField->Delete();
                textField->cursorBlinkTime = 0.0f;
                textField->cursorVisible = true;
            }

            if (Input::KeyTapped(GLFW_KEY_LEFT) || (Input::KeyHeld(GLFW_KEY_LEFT) && textField->cursorBlinkTime > 0.3f)) {
                textField->MoveCursorLeft(shift);
                textField->cursorBlinkTime = 0.0f;
                textField->cursorVisible = true;
            }

            if (Input::KeyTapped(GLFW_KEY_RIGHT) || (Input::KeyHeld(GLFW_KEY_RIGHT) && textField->cursorBlinkTime > 0.3f)) {
                textField->MoveCursorRight(shift);
                textField->cursorBlinkTime = 0.0f;
                textField->cursorVisible = true;
            }

            if (Input::KeyTapped(GLFW_KEY_HOME)) {
                textField->MoveCursorToStart(shift);
                textField->cursorBlinkTime = 0.0f;
                textField->cursorVisible = true;
            }

            if (Input::KeyTapped(GLFW_KEY_END)) {
                textField->MoveCursorToEnd(shift);
                textField->cursorBlinkTime = 0.0f;
                textField->cursorVisible = true;
            }

            if (ctrl && Input::KeyTapped(GLFW_KEY_A)) {
                textField->SelectAll();
                textField->cursorBlinkTime = 0.0f;
                textField->cursorVisible = true;
            }

            if (ctrl && Input::KeyTapped(GLFW_KEY_C)) {
                if (textField->HasSelection()) {
                    size_t start = textField->GetSelectionMin();
                    size_t end = textField->GetSelectionMax();
                    std::string selectedText = textField->text.substr(start, end - start);
                    glfwSetClipboardString(window, selectedText.c_str());
                }
            }

            if (ctrl && Input::KeyTapped(GLFW_KEY_X)) {
                if (textField->HasSelection()) {
                    size_t start = textField->GetSelectionMin();
                    size_t end = textField->GetSelectionMax();
                    std::string selectedText = textField->text.substr(start, end - start);
                    glfwSetClipboardString(window, selectedText.c_str());
                    textField->DeleteSelection();
                    textField->cursorBlinkTime = 0.0f;
                    textField->cursorVisible = true;
                }
            }

            if (ctrl && Input::KeyTapped(GLFW_KEY_V)) {
                const char* clipboardText = glfwGetClipboardString(window);
                if (clipboardText) {
                    std::string pasteText(clipboardText);

                    if (textField->validator) {
                        std::string testText = textField->text;
                        if (textField->HasSelection()) {
                            size_t start = textField->GetSelectionMin();
                            size_t end = textField->GetSelectionMax();
                            testText.erase(start, end - start);
                            testText.insert(start, pasteText);
                        }
                        else {
                            testText.insert(textField->cursorPosition, pasteText);
                        }

                        if (!textField->validator(testText)) {
                            pasteText.clear();
                        }
                    }

                    if (!pasteText.empty()) {
                        textField->InsertText(pasteText);
                        textField->cursorBlinkTime = 0.0f;
                        textField->cursorVisible = true;
                    }
                }
            }

            if (Input::KeyTapped(GLFW_KEY_ENTER) || Input::KeyTapped(GLFW_KEY_KP_ENTER)) {
                if (textField->onSubmit) {
                    textField->onSubmit(textField->text);
                }
            }

            if (Input::KeyTapped(GLFW_KEY_ESCAPE)) {
                ClearFocus(entityManager);
            }
        }
    }

    // Update all text fields (cursor blinking)
    auto query = entityManager.CreateQuery<UIElement, UITextField>();
    for (auto [entity, element, textField] : query) {
        if (!element->isVisible) continue;
        UpdateTextField(entityManager, entity, element, textField, deltaTime);
    }

    Input::BlockInputForUI(focusedTextField != 0);
}

void UIUpdateSystem::UpdateScreenSize(int width, int height) {
    screenWidth = width;
    screenHeight = height;
}

void UIUpdateSystem::OnMouseMove(float x, float y) {
    mousePosition = glm::vec2(x, y);
}

void UIUpdateSystem::OnMouseDown(float x, float y) {
    mousePosition = glm::vec2(x, y);
    mouseDown = true;
}

void UIUpdateSystem::OnMouseUp(float x, float y) {
    mousePosition = glm::vec2(x, y);
    mouseDown = false;
}

void UIUpdateSystem::SetFocus(EntityManager& entityManager, Entity entity) {
    if (focusedTextField == entity) return;

    // Clear focus from previous field
    if (focusedTextField != 0) {
        auto prevField = entityManager.GetComponent<UITextField>(focusedTextField);
        if (prevField) {
            prevField->state = TextFieldState::NORMAL;
            if (prevField->onFocusLost) {
                prevField->onFocusLost();
            }
        }
    }

    // Set focus to new field
    focusedTextField = entity;
    auto textField = entityManager.GetComponent<UITextField>(entity);
    if (textField) {
        textField->state = TextFieldState::FOCUSED;
        textField->cursorBlinkTime = 0.0f;
        textField->cursorVisible = true;

        if (textField->onFocusGained) {
            textField->onFocusGained();
        }
    }
}

void UIUpdateSystem::ClearFocus(EntityManager& entityManager) {
    if (focusedTextField != 0) {
        auto textField = entityManager.GetComponent<UITextField>(focusedTextField);
        if (textField) {
            textField->state = TextFieldState::NORMAL;
            if (textField->onFocusLost) {
                textField->onFocusLost();
            }
        }
        focusedTextField = 0;
    }
}

void UIUpdateSystem::UpdateTextField(EntityManager& entityManager, Entity entity,
    UIElement* element, UITextField* textField, float deltaTime) {
    if (!textField->isInteractable) {
        textField->state = TextFieldState::DISABLED;
        return;
    }

    // Update cursor blink (this is the main purpose of this function now)
    if (textField->state == TextFieldState::FOCUSED) {
        textField->cursorBlinkTime += deltaTime;
        if (textField->cursorBlinkTime >= cursorBlinkInterval) {
            textField->cursorVisible = !textField->cursorVisible;
            textField->cursorBlinkTime = 0.0f;
        }
    }
    else {
        textField->cursorVisible = false;
        textField->cursorBlinkTime = 0.0f;
    }

    // NOTE: Mouse click handling is now done at the start of Update()
    // to avoid issues with Input::KeyTapped vs Input::MousePressed
}

void UIUpdateSystem::HandleTextFieldClick(EntityManager& entityManager, Entity entity,
    UIElement* element, UITextField* textField, const glm::vec2& mousePos) {
    // Get local mouse position relative to text field
    glm::vec2 screenPos = element->GetScreenPosition(screenWidth, screenHeight);
    glm::vec2 localMousePos = mousePos - screenPos;


    // Calculate cursor position from mouse
    size_t newCursorPos = GetCursorPositionFromMouse(textField, element, localMousePos);
    textField->cursorPosition = newCursorPos;
    textField->ClearSelection();
    textField->cursorBlinkTime = 0.0f;
    textField->cursorVisible = true;
}

size_t UIUpdateSystem::GetCursorPositionFromMouse(const UITextField* textField,
    const UIElement* element,
    const glm::vec2& localMousePos) {
    if (!fontManager || textField->text.empty()) {
        return 0;
    }

    // Account for padding
    float xPos = localMousePos.x - textField->padding;

    if (xPos <= 0.0f) {
        return 0;
    }

    // Get display text
    std::string displayText = textField->GetDisplayText();

    // Calculate scale
    const Character* refChar = fontManager->GetCharacter(textField->fontName, 'H');
    if (!refChar) refChar = fontManager->GetCharacter(textField->fontName, 'A');
    float loadedFontSize = refChar ? static_cast<float>(refChar->size.y) : 48.0f;
    float scale = textField->fontSize / loadedFontSize;

    // Calculate which character the mouse is closest to
    float currentX = 0.0f;

    for (size_t i = 0; i < displayText.length(); i++) {
        const Character* ch = fontManager->GetCharacter(textField->fontName, displayText[i]);
        if (!ch) continue;

        float charWidth = (ch->advance >> 6) * scale;

        // Check if mouse is in first half or second half of character
        if (xPos < currentX + charWidth * 0.5f) {
            return i;
        }

        currentX += charWidth;

        if (xPos < currentX) {
            return i + 1;
        }
    }

    return textField->text.length();
}