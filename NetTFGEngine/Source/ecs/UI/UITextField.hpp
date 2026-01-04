#ifndef UITEXTFIELD_HPP
#define UITEXTFIELD_HPP

#include "ecs/ecs_common.hpp"
#include <string>
#include <functional>
#include "OpenGL/OpenGLIncludes.hpp"

enum class TextFieldState {
    NORMAL,
    FOCUSED,
    DISABLED
};

class UITextField : public IComponent {
public:
    UITextField(const std::string& placeholder = "", size_t maxLength = 256)
        : id("")
        ,text("")
        , placeholderText(placeholder)
        , maxLength(maxLength)
        , state(TextFieldState::NORMAL)
        , isInteractable(true)
        , isPassword(false)
        , cursorPosition(0)
        , selectionStart(0)
        , selectionEnd(0)
        , cursorBlinkTime(0.0f)
        , cursorVisible(true)
        , textColor(0.0f, 0.0f, 0.0f, 1.0f)
        , placeholderColor(0.5f, 0.5f, 0.5f, 1.0f)
        , backgroundColor(1.0f, 1.0f, 1.0f, 1.0f)
        , focusColor(0.9f, 0.95f, 1.0f, 1.0f)
        , borderColor(0.7f, 0.7f, 0.7f, 1.0f)
        , focusBorderColor(0.3f, 0.5f, 0.9f, 1.0f)
        , selectionColor(0.4f, 0.6f, 1.0f, 0.5f)
        , fontSize(16.0f)
        , padding(8.0f)
        , fontName("default")
    {
    }

    std::string id;

    // Text content
    std::string text;
    std::string placeholderText;
    size_t maxLength;

    // State
    TextFieldState state;
    bool isInteractable;
    bool isPassword;  // Display text as asterisks

    // Cursor and selection
    size_t cursorPosition;
    size_t selectionStart;
    size_t selectionEnd;
    float cursorBlinkTime;
    bool cursorVisible;

    // Visual properties
    glm::vec4 textColor;
    glm::vec4 placeholderColor;
    glm::vec4 backgroundColor;
    glm::vec4 focusColor;
    glm::vec4 borderColor;
    glm::vec4 focusBorderColor;
    glm::vec4 selectionColor;
    float fontSize;
    float padding;
    std::string fontName;

    // Callbacks
    std::function<void(const std::string&)> onTextChanged;
    std::function<void()> onFocusGained;
    std::function<void()> onFocusLost;
    std::function<void(const std::string&)> onSubmit;  // Called on Enter key

    // Validation callback - return true if text is valid
    std::function<bool(const std::string&)> validator;

    // Helper methods
    bool HasSelection() const {
        return selectionStart != selectionEnd;
    }

    size_t GetSelectionMin() const {
        return std::min(selectionStart, selectionEnd);
    }

    size_t GetSelectionMax() const {
        return std::max(selectionStart, selectionEnd);
    }

    void ClearSelection() {
        selectionStart = selectionEnd = cursorPosition;
    }

    void SelectAll() {
        selectionStart = 0;
        selectionEnd = text.length();
        cursorPosition = text.length();
    }

    std::string GetDisplayText() const {
        if (isPassword && !text.empty()) {
            return std::string(text.length(), '*');
        }
        return text;
    }

    glm::vec4 GetCurrentBackgroundColor() const {
        switch (state) {
        case TextFieldState::FOCUSED:
            return focusColor;
        case TextFieldState::DISABLED:
            return backgroundColor * glm::vec4(0.9f, 0.9f, 0.9f, 1.0f);
        default:
            return backgroundColor;
        }
    }

    glm::vec4 GetCurrentBorderColor() const {
        switch (state) {
        case TextFieldState::FOCUSED:
            return focusBorderColor;
        case TextFieldState::DISABLED:
            return borderColor * glm::vec4(0.7f, 0.7f, 0.7f, 1.0f);
        default:
            return borderColor;
        }
    }

    // Text manipulation
    void InsertText(const std::string& insertText) {
        if (!isInteractable || state == TextFieldState::DISABLED) return;

        // Delete selection if any
        if (HasSelection()) {
            DeleteSelection();
        }

        // Check if we can insert
        if (text.length() + insertText.length() > maxLength) {
            return;
        }

        // Insert text at cursor
        text.insert(cursorPosition, insertText);
        cursorPosition += insertText.length();
        ClearSelection();

        if (onTextChanged) {
            onTextChanged(text);
        }
    }

    void DeleteSelection() {
        if (!HasSelection()) return;

        size_t start = GetSelectionMin();
        size_t end = GetSelectionMax();
        text.erase(start, end - start);
        cursorPosition = start;
        ClearSelection();

        if (onTextChanged) {
            onTextChanged(text);
        }
    }

    void Backspace() {

        Debug::Info("UITextField") << "Backspace at position " << cursorPosition << "\n";

        if (!isInteractable || state == TextFieldState::DISABLED) return;


        if (HasSelection()) {
            DeleteSelection();
        }
        else if (cursorPosition > 0) {
            text.erase(cursorPosition - 1, 1);
            cursorPosition--;

            if (onTextChanged) {
                onTextChanged(text);
            }
        }
    }

    void Delete() {
        if (!isInteractable || state == TextFieldState::DISABLED) return;

        if (HasSelection()) {
            DeleteSelection();
        }
        else if (cursorPosition < text.length()) {
            text.erase(cursorPosition, 1);

            if (onTextChanged) {
                onTextChanged(text);
            }
        }
    }

    void MoveCursorLeft(bool selecting = false) {
        if (cursorPosition > 0) {
            cursorPosition--;
            if (selecting) {
                selectionEnd = cursorPosition;
            }
            else {
                ClearSelection();
            }
        }
    }

    void MoveCursorRight(bool selecting = false) {
        if (cursorPosition < text.length()) {
            cursorPosition++;
            if (selecting) {
                selectionEnd = cursorPosition;
            }
            else {
                ClearSelection();
            }
        }
    }

    void MoveCursorToStart(bool selecting = false) {
        cursorPosition = 0;
        if (selecting) {
            selectionEnd = cursorPosition;
        }
        else {
            ClearSelection();
        }
    }

    void MoveCursorToEnd(bool selecting = false) {
        cursorPosition = text.length();
        if (selecting) {
            selectionEnd = cursorPosition;
        }
        else {
            ClearSelection();
        }
    }
};

#endif // UITEXTFIELD_HPP