#include "UIUpdateSystem.hpp"
#include "Utils/Input.hpp"
#include <algorithm>
#include <cmath>
#include <iostream>

// Static pointer for callback access
static UIUpdateSystem* g_UIUpdateSystemInstance = nullptr;

UIUpdateSystem::UIUpdateSystem(int refWidth, int refHeight, GLFWwindow* win, FontManager* fontMgr)
    : refWidth(refWidth)
    , refHeight(refHeight)
    , screenWidth(refWidth)
    , screenHeight(refHeight)
    , mousePosition(0.0f)
    , mouseDown(false)
    , window(win)
    , fontManager(fontMgr)
    , cachedEntityManager(nullptr)
    , focusedTextField(0)
    , cursorBlinkInterval(0.53f)
    , activeSlider(0)
    , focusedSlider(0)
    , openDropdown(0)
    , prevMouseDown(false)
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

    // Convert mouse from window/screen coords to reference coords so that
    // Contains(refW, refH) matches the reference-space positions the renderer draws.
    float refScaleX = (screenWidth > 0) ? (float)refWidth / (float)screenWidth : 1.0f;
    float refScaleY = (screenHeight > 0) ? (float)refHeight / (float)screenHeight : 1.0f;
    double refMouseX = mouseX * refScaleX;
    double refMouseY = mouseY * refScaleY;
    glm::vec2 refMouse(static_cast<float>(refMouseX), static_cast<float>(refMouseY));

    // Press edge. Input::MousePressed() stays true while the button is held
    // (PRESSED || HELD), so using it directly would toggle a dropdown once
    // per frame during a single click. Input's own one-frame edge would be
    // MousePressed() && !MouseHeld(), but comparing against the previous
    // state also holds up if Update() runs more than once per Input::Update().
    bool mouseIsDown = IsMouseLeftDown();
    bool mouseJustPressed = mouseIsDown && !prevMouseDown;

    // Dropdowns and sliders get the click first: an open popup is drawn on top
    // of the rest of the UI, so it must also be hit tested first.
    bool clickConsumed = false;
    if (mouseJustPressed) {
        clickConsumed = HandleDropdownClick(entityManager, refMouse);
        if (!clickConsumed) {
            clickConsumed = HandleSliderClick(entityManager, refMouse);
        }
        if (clickConsumed && focusedTextField != 0) {
            ClearFocus(entityManager);
        }
    }

    if (mouseJustPressed && !clickConsumed) {
        bool clickedAnyField = false;
        Entity clickedField = 0;
        bool clickedButton = false;

        // Check buttons first
        auto buttonQuery = entityManager.CreateQuery<UIElement, UIButton>();
        for (auto [entity, element, button] : buttonQuery) {
            if (!element->isVisible || !button->isInteractable) continue;
            if (element->Contains({ refMouseX, refMouseY }, refWidth, refHeight)) {
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
                if (element->Contains({ refMouseX, refMouseY }, refWidth, refHeight)) {
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

        bool isInside = element->Contains({ refMouseX, refMouseY }, refWidth, refHeight);
        ButtonState previousState = button->state;

        if (mouseIsDown && isInside) {
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

    // Sliders: dragging + hover states
    UpdateSliders(entityManager, refMouse, mouseIsDown, deltaTime);

    // Dropdowns: header hover + highlighted row under the mouse
    UpdateDropdowns(entityManager, refMouse);

    // Mouse wheel scrolls the open list
    if (openDropdown != 0) {
        double scrollDeltaX = 0.0, scrollDeltaY = 0.0;
        Input::GetScrollDelta(scrollDeltaX, scrollDeltaY);
        if (scrollDeltaY != 0.0) {
            OnScroll(static_cast<float>(scrollDeltaY));
        }
    }

    // Keyboard: an open dropdown takes priority, then a focused slider.
    // Both are skipped while a text field has focus so typing is never stolen.
    if (focusedTextField == 0) {
        if (openDropdown != 0) {
            HandleDropdownKeyboard(entityManager);
        }
        else if (focusedSlider != 0) {
            HandleSliderKeyboard(entityManager);
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

    if (focusedTextField != 0) {
        focusedSlider = 0;
    }

    // Block gameplay input while the UI is being used
    Input::BlockInputForUI(IsInteractingWithUI());

    // Must be the last thing in Update(): everything above reads the edge
    prevMouseDown = mouseIsDown;
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
    // Convert mouse from screen coords to reference coords, then get local position.
    float refScaleX = (screenWidth > 0) ? (float)refWidth / (float)screenWidth : 1.0f;
    float refScaleY = (screenHeight > 0) ? (float)refHeight / (float)screenHeight : 1.0f;
    glm::vec2 refMouse(mousePos.x * refScaleX, mousePos.y * refScaleY);
    glm::vec2 screenPos = element->GetScreenPosition(refWidth, refHeight);
    glm::vec2 localMousePos = refMouse - screenPos;


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

// ---------------------------------------------------------------------------
// Mouse helpers
// ---------------------------------------------------------------------------

bool UIUpdateSystem::IsMouseLeftDown() const {
    // Input::MousePressed is PRESSED || HELD, i.e. "the button is down",
    // which is what dragging needs. The press edge is derived in Update().
    return Input::MousePressed(GLFW_MOUSE_BUTTON_LEFT);
}

bool UIUpdateSystem::OnScroll(float yOffset) {
    if (openDropdown == 0 || !cachedEntityManager) return false;

    auto dropdown = cachedEntityManager->GetComponent<UIDropdown>(openDropdown);
    if (!dropdown || !dropdown->isOpen) return false;

    int rows = -static_cast<int>(std::round(yOffset));
    if (rows == 0) rows = (yOffset > 0.0f) ? -1 : 1;

    dropdown->Scroll(rows);
    return true;
}

// ---------------------------------------------------------------------------
// Dropdown input
// ---------------------------------------------------------------------------

void UIUpdateSystem::CloseAllDropdowns(EntityManager& entityManager) {
    auto query = entityManager.CreateQuery<UIDropdown>();
    for (auto [entity, dropdown] : query) {
        dropdown->Close();
    }
    openDropdown = 0;
}

bool UIUpdateSystem::HandleDropdownClick(EntityManager& entityManager, const glm::vec2& refMouse) {
    // An open popup behaves like a modal: it is hit tested before anything
    // else, and any click outside of it just dismisses it.
    if (openDropdown != 0) {
        auto element = entityManager.GetComponent<UIElement>(openDropdown);
        auto dropdown = entityManager.GetComponent<UIDropdown>(openDropdown);

        if (!element || !dropdown || !dropdown->isOpen) {
            openDropdown = 0;
        }
        else {
            glm::vec2 pos = element->GetScreenPosition(refWidth, refHeight);

            int index = dropdown->GetItemIndexAtPoint(refMouse, pos, element->size, refHeight);
            if (index >= 0) {
                dropdown->SelectIndex(index);
                dropdown->Close();
                openDropdown = 0;
                return true;
            }

            // Border or scrollbar: swallow the click, keep the list open
            if (dropdown->ContainsList(refMouse, pos, element->size, refHeight)) {
                return true;
            }

            // Anywhere else (including the header): dismiss
            dropdown->Close();
            openDropdown = 0;
            return true;
        }
    }

    // No popup open: check the headers
    auto query = entityManager.CreateQuery<UIElement, UIDropdown>();
    for (auto [entity, element, dropdown] : query) {
        if (!element->isVisible || !dropdown->isInteractable) continue;
        if (!element->Contains(refMouse, refWidth, refHeight)) continue;

        dropdown->Open();
        if (dropdown->isOpen) {
            openDropdown = entity;
        }
        return true;
    }

    return false;
}

void UIUpdateSystem::UpdateDropdowns(EntityManager& entityManager, const glm::vec2& refMouse) {
    auto query = entityManager.CreateQuery<UIElement, UIDropdown>();
    for (auto [entity, element, dropdown] : query) {
        if (!element->isVisible || !dropdown->isInteractable) {
            if (dropdown->isOpen) {
                dropdown->Close();
                if (openDropdown == entity) openDropdown = 0;
            }
            if (!dropdown->isInteractable) dropdown->state = DropdownState::DISABLED;
            continue;
        }

        glm::vec2 pos = element->GetScreenPosition(refWidth, refHeight);

        if (dropdown->isOpen) {
            // Opened from code (dropdown->Open()): adopt it and close the old one
            if (openDropdown != entity) {
                if (openDropdown != 0) {
                    auto other = entityManager.GetComponent<UIDropdown>(openDropdown);
                    if (other) other->Close();
                }
                openDropdown = entity;
            }

            dropdown->state = DropdownState::OPEN;
            dropdown->ClampScroll();

            int index = dropdown->GetItemIndexAtPoint(refMouse, pos, element->size, refHeight);
            if (index >= 0) {
                dropdown->hoveredIndex = index;
            }
            continue;
        }

        if (openDropdown == entity) openDropdown = 0;

        bool isInside = (openDropdown == 0) && element->Contains(refMouse, refWidth, refHeight);
        dropdown->state = isInside ? DropdownState::HOVERED : DropdownState::NORMAL;
    }
}

void UIUpdateSystem::HandleDropdownKeyboard(EntityManager& entityManager) {
    if (openDropdown == 0) return;

    auto dropdown = entityManager.GetComponent<UIDropdown>(openDropdown);
    if (!dropdown || !dropdown->isOpen) {
        openDropdown = 0;
        return;
    }

    if (Input::KeyTapped(GLFW_KEY_DOWN))  dropdown->MoveHighlight(1);
    if (Input::KeyTapped(GLFW_KEY_UP))    dropdown->MoveHighlight(-1);
    if (Input::KeyTapped(GLFW_KEY_PAGE_DOWN)) dropdown->MoveHighlight(dropdown->GetVisibleItemCount());
    if (Input::KeyTapped(GLFW_KEY_PAGE_UP))   dropdown->MoveHighlight(-dropdown->GetVisibleItemCount());

    if (Input::KeyTapped(GLFW_KEY_HOME)) {
        dropdown->hoveredIndex = 0;
        dropdown->EnsureVisible(0);
    }
    if (Input::KeyTapped(GLFW_KEY_END)) {
        int last = dropdown->GetOptionCount() - 1;
        dropdown->hoveredIndex = last;
        dropdown->EnsureVisible(last);
    }

    if (Input::KeyTapped(GLFW_KEY_ENTER) || Input::KeyTapped(GLFW_KEY_KP_ENTER)) {
        if (dropdown->hoveredIndex >= 0) {
            dropdown->SelectIndex(dropdown->hoveredIndex);
        }
        dropdown->Close();
        openDropdown = 0;
        return;
    }

    if (Input::KeyTapped(GLFW_KEY_ESCAPE)) {
        dropdown->Close();
        openDropdown = 0;
    }
}

// ---------------------------------------------------------------------------
// Slider input
// ---------------------------------------------------------------------------

bool UIUpdateSystem::HandleSliderClick(EntityManager& entityManager, const glm::vec2& refMouse) {
    auto query = entityManager.CreateQuery<UIElement, UISlider>();
    for (auto [entity, element, slider] : query) {
        if (!element->isVisible || !slider->isInteractable) continue;

        glm::vec2 pos = element->GetScreenPosition(refWidth, refHeight);
        if (!slider->ContainsPoint(refMouse, pos, element->size)) continue;

        activeSlider = entity;
        focusedSlider = entity;
        slider->state = SliderState::DRAGGING;
        if (slider->onDragStart) slider->onDragStart();

        // Clicking the track jumps to that value; grabbing the handle does not
        if (!slider->ContainsHandle(refMouse, pos, element->size)) {
            slider->SetValue(slider->GetValueFromPoint(refMouse, pos, element->size));
        }
        return true;
    }
    return false;
}

void UIUpdateSystem::UpdateSliders(EntityManager& entityManager, const glm::vec2& refMouse,
    bool mouseIsDown, float deltaTime) {
    (void)deltaTime;

    // Active drag: keep following the mouse even outside the element bounds
    if (activeSlider != 0) {
        auto element = entityManager.GetComponent<UIElement>(activeSlider);
        auto slider = entityManager.GetComponent<UISlider>(activeSlider);

        if (!element || !slider || !slider->isInteractable || !element->isVisible) {
            activeSlider = 0;
        }
        else if (mouseIsDown) {
            glm::vec2 pos = element->GetScreenPosition(refWidth, refHeight);
            slider->SetValue(slider->GetValueFromPoint(refMouse, pos, element->size));
            slider->state = SliderState::DRAGGING;
        }
        else {
            EndSliderDrag(entityManager);
        }
    }

    // Hover states
    auto query = entityManager.CreateQuery<UIElement, UISlider>();
    for (auto [entity, element, slider] : query) {
        if (!slider->isInteractable) {
            slider->state = SliderState::DISABLED;
            continue;
        }
        if (!element->isVisible || entity == activeSlider) continue;

        glm::vec2 pos = element->GetScreenPosition(refWidth, refHeight);
        bool isInside = (openDropdown == 0) &&
            slider->ContainsPoint(refMouse, pos, element->size);

        slider->state = isInside ? SliderState::HOVERED : SliderState::NORMAL;
    }
}

void UIUpdateSystem::EndSliderDrag(EntityManager& entityManager) {
    if (activeSlider == 0) return;

    auto slider = entityManager.GetComponent<UISlider>(activeSlider);
    if (slider) {
        slider->state = SliderState::NORMAL;
        if (slider->onDragEnd) slider->onDragEnd(slider->value);
    }
    activeSlider = 0;
}

void UIUpdateSystem::HandleSliderKeyboard(EntityManager& entityManager) {
    if (focusedSlider == 0) return;

    auto element = entityManager.GetComponent<UIElement>(focusedSlider);
    auto slider = entityManager.GetComponent<UISlider>(focusedSlider);
    if (!element || !slider || !element->isVisible || !slider->isInteractable) {
        focusedSlider = 0;
        return;
    }

    bool horizontal = (slider->orientation == SliderOrientation::HORIZONTAL);
    int decreaseKey = horizontal ? GLFW_KEY_LEFT : GLFW_KEY_DOWN;
    int increaseKey = horizontal ? GLFW_KEY_RIGHT : GLFW_KEY_UP;

    if (Input::KeyTapped(decreaseKey)) slider->StepValue(-1);
    if (Input::KeyTapped(increaseKey)) slider->StepValue(1);
    if (Input::KeyTapped(GLFW_KEY_HOME)) slider->SetValue(slider->minValue);
    if (Input::KeyTapped(GLFW_KEY_END))  slider->SetValue(slider->maxValue);
    if (Input::KeyTapped(GLFW_KEY_ESCAPE)) focusedSlider = 0;
}