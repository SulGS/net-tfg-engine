#include "UIUpdateSystem.hpp"
#include "Utils/Input.hpp"
#include "Utils/InputMap.hpp"
#include "Utils/Utf8.hpp"
#include <algorithm>
#include <cfloat>
#include <climits>
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

    // Press edge: MousePressed() stays true while held, so it would toggle a dropdown every frame. Comparing against
    // the previous state (vs Input's own edge) also holds if Update() runs more than once per Input::Update().
    bool mouseIsDown = IsMouseLeftDown();
    bool mouseJustPressed = mouseIsDown && !prevMouseDown;

    // The mouse takes over hover/highlight when it's actually used; the gamepad hands it back to navigation.
    {
        double mouseDX = 0.0, mouseDY = 0.0;
        Input::GetMouseDelta(mouseDX, mouseDY);
        if (mouseJustPressed || std::abs(mouseDX) + std::abs(mouseDY) > 4.0) navActive = false;
        if (Input::LastDevice() == Input::Device::Gamepad) navActive = true;
    }

    // Scroll views first: everything below hit tests (and the renderer draws) the children where this lays them out.
    UpdateScrollViews(entityManager, refMouse, mouseIsDown, deltaTime);

    // Dropdowns and sliders get the click first: an open popup is drawn on top
    // of the rest of the UI, so it must also be hit tested first. Then scroll bars, which sit over their content.
    bool clickConsumed = false;
    if (mouseJustPressed) {
        clickConsumed = HandleDropdownClick(entityManager, refMouse);
        if (!clickConsumed) {
            clickConsumed = HandleScrollBarClick(entityManager, refMouse);
        }
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
            if (HitTest(entityManager, entity, element, refMouse)) {
                navFocus = entity;
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
                if (HitTest(entityManager, entity, element, refMouse)) {
                    clickedAnyField = true;
                    clickedField = entity;
                    break;
                }
            }

            if (clickedAnyField) {
                navFocus = clickedField;
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

        // Navigating: the focused button is the "hovered" one, wherever the (maybe hidden) pointer is.
        bool isInside = navActive
            ? (entity == navFocus)
            : HitTest(entityManager, entity, element, refMouse);
        ButtonState previousState = button->state;

        if (mouseIsDown && isInside && !navActive) {
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

    // Taken before the handlers below: whatever they close this frame (a dropdown picking with Accept, a text field
    // left with Back) must not also be read by the navigation as a new Accept/Back.
    const bool navBlocked = focusedTextField != 0 || openDropdown != 0 || activeSlider != 0;

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

            // Esc, or B on a gamepad (which can't type, but can focus the field with A).
            if (InputMap::Get().Tapped(UIAction::Back)) {
                ClearFocus(entityManager);
            }
        }
    }

    // Arrows / D-pad / stick move the focus, Enter / A activates it. After the text field block: the Enter that
    // focuses a field must not also submit it this frame.
    if (!navBlocked) {
        UpdateNavigation(entityManager);
    }
    ApplyNavHighlight(entityManager);

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
    const Character* refChar = fontManager->GetCharacter(textField->fontName, U'H');
    if (!refChar) refChar = fontManager->GetCharacter(textField->fontName, U'A');
    float loadedFontSize = refChar ? static_cast<float>(refChar->size.y) : 48.0f;
    float scale = textField->fontSize / loadedFontSize;

    // Calculate which character the mouse is closest to
    float currentX = 0.0f;

    // Byte offsets, like cursorPosition: i is where the glyph starts, next where it ends.
    for (size_t i = 0, next = 0; i < displayText.length(); i = next) {
        const Character* ch = fontManager->GetCharacter(textField->fontName, Utf8::Next(displayText, next));
        if (!ch) continue;

        float charWidth = (ch->advance >> 6) * scale;

        // Check if mouse is in first half or second half of character
        if (xPos < currentX + charWidth * 0.5f) {
            return i;
        }

        currentX += charWidth;

        if (xPos < currentX) {
            return next;
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
        if (!HitTest(entityManager, entity, element, refMouse)) continue;

        navFocus = entity;
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

        bool isInside = (openDropdown == 0) && HitTest(entityManager, entity, element, refMouse);
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

    // Arrows, D-pad or stick (repeating while held); Enter / A picks, Esc / B closes.
    InputMap& in = InputMap::Get();
    if (in.Repeated(UIAction::Down)) dropdown->MoveHighlight(1);
    if (in.Repeated(UIAction::Up))   dropdown->MoveHighlight(-1);
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

    if (in.Tapped(UIAction::Accept)) {
        if (dropdown->hoveredIndex >= 0) {
            dropdown->SelectIndex(dropdown->hoveredIndex);
        }
        dropdown->Close();
        openDropdown = 0;
        return;
    }

    if (in.Tapped(UIAction::Back)) {
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
        if (!UIScroll::IsPointVisible(entityManager, entity, refMouse, refWidth, refHeight)) continue;

        activeSlider = entity;
        focusedSlider = entity;
        navFocus = entity;   // arrows then adjust it (UpdateNavigation)
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
            slider->ContainsPoint(refMouse, pos, element->size) &&
            UIScroll::IsPointVisible(entityManager, entity, refMouse, refWidth, refHeight);

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

    // Arrow steps are UpdateNavigation's (a clicked slider is also the navigation focus); only the jumps are here.
    if (Input::KeyTapped(GLFW_KEY_HOME)) slider->SetValue(slider->minValue);
    if (Input::KeyTapped(GLFW_KEY_END))  slider->SetValue(slider->maxValue);
    if (Input::KeyTapped(GLFW_KEY_ESCAPE)) focusedSlider = 0;
}

// ---------------------------------------------------------------------------
// Keyboard / gamepad navigation
// ---------------------------------------------------------------------------

bool UIUpdateSystem::IsNavigable(EntityManager& entityManager, Entity entity) const {
    if (entity == 0) return false;
    const UIElement* element = entityManager.GetComponent<UIElement>(entity);
    if (!element || !element->isVisible || element->navSkip) return false;

    if (const UIButton* b = entityManager.GetComponent<UIButton>(entity)) return b->isInteractable;
    if (const UISlider* s = entityManager.GetComponent<UISlider>(entity)) return s->isInteractable;
    if (const UIDropdown* d = entityManager.GetComponent<UIDropdown>(entity)) return d->isInteractable;
    if (const UITextField* t = entityManager.GetComponent<UITextField>(entity)) return t->isInteractable;
    return false;
}

bool UIUpdateSystem::GetNavCenter(EntityManager& entityManager, Entity entity, glm::vec2& center) const {
    const UIElement* element = entityManager.GetComponent<UIElement>(entity);
    if (!element) return false;
    center = element->GetScreenPosition(refWidth, refHeight) + element->size * 0.5f;
    return true;
}

Entity UIUpdateSystem::PickNavTarget(EntityManager& entityManager, const glm::vec2* nearPoint) const {
    Entity best = 0, bestDefault = 0;
    int bestLayer = INT_MIN, bestDefaultLayer = INT_MIN;
    glm::vec2 bestPos(0.0f);
    float bestNearDist = FLT_MAX;
    Entity bestNear = 0;

    auto query = entityManager.CreateQuery<UIElement>();
    for (auto [entity, element] : query) {
        if (!IsNavigable(entityManager, entity)) continue;
        // Picked from scratch: only what can be seen. (Scrolled-away widgets stay reachable by moving onto them.)
        if (UIScroll::IsFullyClipped(entityManager, entity, refWidth, refHeight)) continue;

        const glm::vec2 pos = element->GetScreenPosition(refWidth, refHeight);

        if (element->navDefault && element->layer > bestDefaultLayer) {
            bestDefault = entity;
            bestDefaultLayer = element->layer;
        }

        if (nearPoint) {
            glm::vec2 c = pos + element->size * 0.5f;
            const glm::vec2 d = c - *nearPoint;
            const float dist = d.x * d.x + d.y * d.y;
            if (dist < bestNearDist) { bestNearDist = dist; bestNear = entity; }
        }

        // Highest layer (the dialog on top), then the top-most, then the left-most.
        const bool better = best == 0
            || element->layer > bestLayer
            || (element->layer == bestLayer && (pos.y < bestPos.y - 0.5f
                || (std::abs(pos.y - bestPos.y) <= 0.5f && pos.x < bestPos.x)));
        if (better) {
            best = entity;
            bestLayer = element->layer;
            bestPos = pos;
        }
    }

    if (bestDefault) return bestDefault;
    if (bestNear) return bestNear;
    return best;
}

Entity UIUpdateSystem::FindNavNeighbour(EntityManager& entityManager, Entity from, glm::vec2 direction) const {
    glm::vec2 origin;
    if (!GetNavCenter(entityManager, from, origin)) return 0;
    const float len = std::sqrt(direction.x * direction.x + direction.y * direction.y);
    if (len <= 0.0f) return 0;
    direction /= len;

    Entity best = 0;
    float bestScore = FLT_MAX;

    auto query = entityManager.CreateQuery<UIElement>();
    for (auto [entity, element] : query) {
        if (entity == from || !IsNavigable(entityManager, entity)) continue;

        const glm::vec2 c = element->GetScreenPosition(refWidth, refHeight) + element->size * 0.5f;
        const glm::vec2 v = c - origin;
        const float along = v.x * direction.x + v.y * direction.y;
        if (along < 1.0f) continue;   // not in that direction at all

        // Sideways offset weighs more than distance: "down" prefers the next row over something far to the side.
        const float across = std::abs(v.x * direction.y - v.y * direction.x);
        const float score = along + 2.5f * across;
        if (score < bestScore) {
            bestScore = score;
            best = entity;
        }
    }
    return best;
}

void UIUpdateSystem::ActivateNavFocus(EntityManager& entityManager) {
    if (UIButton* button = entityManager.GetComponent<UIButton>(navFocus)) {
        if (button->isInteractable && button->onClick) button->onClick();
        return;
    }
    if (UIDropdown* dropdown = entityManager.GetComponent<UIDropdown>(navFocus)) {
        if (openDropdown != 0 && openDropdown != navFocus) {
            if (UIDropdown* other = entityManager.GetComponent<UIDropdown>(openDropdown)) other->Close();
        }
        dropdown->Open();
        openDropdown = dropdown->isOpen ? navFocus : 0;
        return;
    }
    if (entityManager.GetComponent<UITextField>(navFocus)) {
        SetFocus(entityManager, navFocus);
    }
}

void UIUpdateSystem::UpdateNavigation(EntityManager& entityManager) {
    InputMap& in = InputMap::Get();

    int dx = 0, dy = 0;
    if (in.Repeated(UIAction::Up)) dy = -1;
    else if (in.Repeated(UIAction::Down)) dy = 1;
    if (in.Repeated(UIAction::Left)) dx = -1;
    else if (in.Repeated(UIAction::Right)) dx = 1;
    const bool accept = in.Tapped(UIAction::Accept);

    RevalidateNavFocus(entityManager);

    if (dx == 0 && dy == 0 && !accept) return;
    navActive = true;

    // Nothing focused yet (or it went away: tab change, dialog closed): this press only brings the focus up, near
    // where it was. It doesn't also move or click, so the player sees what's selected before anything happens.
    if (navFocus == 0) {
        navFocus = PickNavTarget(entityManager, hasLastNavCenter ? &lastNavCenter : nullptr);
        if (navFocus != 0) hasLastNavCenter = GetNavCenter(entityManager, navFocus, lastNavCenter);
        return;
    }

    // The focus was scrolled out of sight (wheel, right stick, bar): same as above, it comes back on something visible
    // in that view, nearest to where it left, instead of moving relative to a widget the player can't see.
    if (UIScroll::IsFullyClipped(entityManager, navFocus, refWidth, refHeight)) {
        glm::vec2 nearPoint(0.0f);
        GetNavCenter(entityManager, navFocus, nearPoint);
        glm::vec4 clip;
        if (UIScroll::GetClipRect(entityManager, navFocus, refWidth, refHeight, clip)) {
            nearPoint.y = std::clamp(nearPoint.y, clip.y, clip.y + clip.w);
        }
        const Entity visible = PickNavTarget(entityManager, &nearPoint);
        if (visible != 0) {
            navFocus = visible;
            hasLastNavCenter = GetNavCenter(entityManager, navFocus, lastNavCenter);
            return;
        }
    }

    // Along the widget's own axis, arrows change its value instead of moving away.
    if (UISlider* slider = entityManager.GetComponent<UISlider>(navFocus)) {
        if (slider->orientation == SliderOrientation::HORIZONTAL && dx != 0) {
            slider->StepValue(dx);
            dx = 0;
        }
        else if (slider->orientation != SliderOrientation::HORIZONTAL && dy != 0) {
            slider->StepValue(-dy);   // up = more
            dy = 0;
        }
    }
    else if (UIDropdown* dropdown = entityManager.GetComponent<UIDropdown>(navFocus)) {
        // Left/right cycle the options without opening the list (quick for the gamepad).
        if (dx != 0 && dropdown->GetOptionCount() > 0) {
            const int current = dropdown->HasSelection() ? dropdown->selectedIndex : 0;
            const int next = std::clamp(current + dx, 0, dropdown->GetOptionCount() - 1);
            dropdown->SelectIndex(next);
            dx = 0;
        }
    }

    if (dx != 0 || dy != 0) {
        const Entity next = FindNavNeighbour(entityManager, navFocus,
            glm::vec2(static_cast<float>(dx), static_cast<float>(dy)));
        if (next != 0) navFocus = next;
        ScrollNavFocusIntoView(entityManager);
    }

    hasLastNavCenter = GetNavCenter(entityManager, navFocus, lastNavCenter);

    if (accept) ActivateNavFocus(entityManager);
}

void UIUpdateSystem::RevalidateNavFocus(EntityManager& entityManager) {
    if (navFocus == 0 || IsNavigable(entityManager, navFocus)) return;
    // The focused widget went away (tab switched, dialog closed). While navigating, move to the nearest one left so
    // the highlight doesn't just vanish; otherwise drop it.
    navFocus = (navActive && hasLastNavCenter) ? PickNavTarget(entityManager, &lastNavCenter) : 0;
    if (navFocus != 0) hasLastNavCenter = GetNavCenter(entityManager, navFocus, lastNavCenter);
}

void UIUpdateSystem::ApplyNavHighlight(EntityManager& entityManager) {
    RevalidateNavFocus(entityManager);
    const Entity shown = navActive ? navFocus : 0;

    if (navFlagged != shown) {
        if (UIElement* old = entityManager.GetComponent<UIElement>(navFlagged)) old->navFocused = false;
        navFlagged = shown;
    }
    if (UIElement* element = entityManager.GetComponent<UIElement>(shown)) element->navFocused = true;

    if (!navActive) return;

    // Sliders and dropdowns take their hover look from the focus too (buttons already do, in Update()).
    auto sliderQuery = entityManager.CreateQuery<UIElement, UISlider>();
    for (auto [entity, element, slider] : sliderQuery) {
        if (!slider->isInteractable || entity == activeSlider) continue;
        slider->state = (entity == navFocus) ? SliderState::HOVERED : SliderState::NORMAL;
    }
    auto dropdownQuery = entityManager.CreateQuery<UIElement, UIDropdown>();
    for (auto [entity, element, dropdown] : dropdownQuery) {
        if (!dropdown->isInteractable || dropdown->isOpen) continue;
        dropdown->state = (entity == navFocus) ? DropdownState::HOVERED : DropdownState::NORMAL;
    }
}
// ---------------------------------------------------------------------------
// Scroll views
// ---------------------------------------------------------------------------

bool UIUpdateSystem::HitTest(EntityManager& entityManager, Entity entity, const UIElement* element,
    const glm::vec2& refMouse) const {
    return element->Contains(refMouse, refWidth, refHeight)
        && UIScroll::IsPointVisible(entityManager, entity, refMouse, refWidth, refHeight);
}

void UIUpdateSystem::UpdateScrollViews(EntityManager& entityManager, const glm::vec2& refMouse, bool mouseIsDown,
    float deltaTime) {
    double wheelX = 0.0, wheelY = 0.0;
    Input::GetScrollDelta(wheelX, wheelY);

    InputMap& in = InputMap::Get();
    // Right stick: up scrolls towards the top. Analog, so a light push reads slowly.
    const float stick = in.Value(UIAction::ScrollDown) - in.Value(UIAction::ScrollUp);
    // An open popup, a text field or a slider drag own the input: the view stays put under them.
    const bool inputFree = openDropdown == 0 && focusedTextField == 0 && activeSlider == 0;

    auto viewQuery = entityManager.CreateQuery<UIElement, UIScrollView>();
    for (auto [viewEntity, viewElement, view] : viewQuery) {
        const glm::vec2 viewPos = viewElement->GetScreenPosition(refWidth, refHeight);
        const glm::vec2 viewSize = viewElement->size;

        // Measure the content: the bottom of the lowest visible child, at scroll 0, below the view's top edge.
        if (view->autoContentHeight) {
            float bottom = 0.0f;
            auto childQuery = entityManager.CreateQuery<UIElement, UIScrollChild>();
            for (auto [childEntity, element, child] : childQuery) {
                if (child->view != viewEntity || !element->isVisible) continue;
                const float baseY = element->GetScreenPosition(refWidth, refHeight).y
                    - element->position.y + child->basePosition.y;
                bottom = std::max(bottom, baseY + element->size.y - viewPos.y);
            }
            view->contentHeight = bottom > 0.0f ? bottom + view->contentPadding : 0.0f;
        }

        const float maxScroll = view->MaxScroll(viewSize.y);
        const bool active = viewElement->isVisible && view->isInteractable && view->CanScroll(viewSize.y);

        if (!active) {
            view->thumbDragging = false;
            view->thumbHovered = false;
        }
        else {
            const glm::vec4 thumb = view->ThumbRect(viewPos, viewSize);
            view->thumbHovered = inputFree && UIScroll::RectContains(thumb, refMouse);

            if (view->thumbDragging) {
                if (!mouseIsDown) {
                    view->thumbDragging = false;
                }
                else {
                    const glm::vec4 track = view->TrackRect(viewPos, viewSize);
                    const float travel = std::max(track.w - thumb.w, 1.0f);
                    const float t = std::clamp((refMouse.y - view->dragGrab - track.y) / travel, 0.0f, 1.0f);
                    view->ScrollTo(t * maxScroll, true);   // the thumb sticks to the pointer: no easing
                }
            }
            else if (inputFree) {
                const bool overView = UIScroll::RectContains(glm::vec4(viewPos, viewSize), refMouse);
                if (overView && wheelY != 0.0) view->ScrollBy(-static_cast<float>(wheelY) * view->wheelStep);
                if (stick != 0.0f) view->ScrollBy(stick * view->stickSpeed * deltaTime);
                // Page keys act on any visible view (they aren't tied to the pointer).
                if (Input::KeyTapped(GLFW_KEY_PAGE_DOWN)) view->ScrollBy(viewSize.y * 0.9f);
                if (Input::KeyTapped(GLFW_KEY_PAGE_UP))   view->ScrollBy(-viewSize.y * 0.9f);
            }
        }

        // Ease towards the target (both kept in range: the content can shrink, e.g. on a tab switch).
        view->targetScroll = std::clamp(view->targetScroll, 0.0f, maxScroll);
        if (view->smoothing <= 0.0f || std::abs(view->targetScroll - view->scroll) < 0.5f) {
            view->scroll = view->targetScroll;
        }
        else {
            const float k = 1.0f - std::exp(-view->smoothing * deltaTime);
            view->scroll += (view->targetScroll - view->scroll) * k;
        }
        view->scroll = std::clamp(view->scroll, 0.0f, maxScroll);
    }

    // Lay the children out. Whole pixels: text and 1px borders would shimmer at fractional offsets while easing.
    auto childQuery = entityManager.CreateQuery<UIElement, UIScrollChild>();
    for (auto [childEntity, element, child] : childQuery) {
        const UIScrollView* view = entityManager.GetComponent<UIScrollView>(child->view);
        const float offset = view ? std::round(view->scroll) : 0.0f;
        element->position = glm::vec2(child->basePosition.x, child->basePosition.y - offset);
    }
}

bool UIUpdateSystem::HandleScrollBarClick(EntityManager& entityManager, const glm::vec2& refMouse) {
    auto viewQuery = entityManager.CreateQuery<UIElement, UIScrollView>();
    for (auto [viewEntity, viewElement, view] : viewQuery) {
        if (!viewElement->isVisible || !view->isInteractable || !view->CanScroll(viewElement->size.y)) continue;

        const glm::vec2 viewPos = viewElement->GetScreenPosition(refWidth, refHeight);
        const glm::vec4 track = view->TrackRect(viewPos, viewElement->size);
        // A little wider than the bar: an 8px target is fiddly to hit.
        const glm::vec4 hitTrack(track.x - 4.0f, track.y, track.z + 8.0f, track.w);
        if (!UIScroll::RectContains(hitTrack, refMouse)) continue;

        const glm::vec4 thumb = view->ThumbRect(viewPos, viewElement->size);
        if (refMouse.y >= thumb.y && refMouse.y <= thumb.y + thumb.w) {
            view->thumbDragging = true;
            view->dragGrab = refMouse.y - thumb.y;
        }
        else {
            // Track: one page towards the click.
            view->ScrollBy((refMouse.y < thumb.y ? -1.0f : 1.0f) * viewElement->size.y * 0.9f);
        }
        return true;
    }
    return false;
}

void UIUpdateSystem::ScrollNavFocusIntoView(EntityManager& entityManager) {
    const UIScrollChild* child = entityManager.GetComponent<UIScrollChild>(navFocus);
    if (!child) return;
    const UIElement* element = entityManager.GetComponent<UIElement>(navFocus);
    const UIElement* viewElement = entityManager.GetComponent<UIElement>(child->view);
    UIScrollView* view = entityManager.GetComponent<UIScrollView>(child->view);
    if (!element || !viewElement || !view) return;

    // The focused widget's span in content coordinates (from the top of the content, scroll 0).
    const float viewTop = viewElement->GetScreenPosition(refWidth, refHeight).y;
    const float top = element->GetScreenPosition(refWidth, refHeight).y - element->position.y
        + child->basePosition.y - viewTop;
    const float bottom = top + element->size.y;
    const float viewHeight = viewElement->size.y;
    const float pad = view->contentPadding;

    // Least movement that shows it whole, with a margin; nothing if it already is.
    float target = view->targetScroll;
    if (top - pad < target) target = top - pad;
    else if (bottom + pad > target + viewHeight) target = bottom + pad - viewHeight;
    view->targetScroll = std::clamp(target, 0.0f, view->MaxScroll(viewHeight));
}
