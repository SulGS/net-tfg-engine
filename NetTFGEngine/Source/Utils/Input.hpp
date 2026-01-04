#ifndef INPUT_HPP
#define INPUT_HPP

#include <GLFW/glfw3.h>
#include <unordered_map>
#include <cctype>

class Input {
public:
    enum class KeyState { NONE, JUST_PRESSED, PRESSED, HELD, JUST_RELEASED ,RELEASED};

    // Call once after you create the GLFW window
    static void Init(GLFWwindow* win) {
        window = win;

        glfwSetKeyCallback(window, keyCallback);
        glfwSetMouseButtonCallback(window, mouseButtonCallback);
        glfwSetCursorPosCallback(window, cursorPosCallback);
        glfwSetScrollCallback(window, scrollCallback);

        // Initialize mouse position
        glfwGetCursorPos(window, &mouseX, &mouseY);
        lastMouseX = mouseX;
        lastMouseY = mouseY;
    }

    // Call once per frame, after glfwPollEvents()
    static void Update() {
        for (auto& kv : keys) {
            if (kv.second == KeyState::JUST_PRESSED)   kv.second = KeyState::PRESSED;
            else if (kv.second == KeyState::PRESSED) kv.second = KeyState::HELD;
            else if (kv.second == KeyState::JUST_RELEASED) kv.second = KeyState::RELEASED;
            else if (kv.second == KeyState::RELEASED) kv.second = KeyState::NONE;
        }

        for (auto& kv : buttons) {
            if (kv.second == KeyState::JUST_PRESSED)   kv.second = KeyState::PRESSED;
            else if (kv.second == KeyState::PRESSED) kv.second = KeyState::HELD;
            else if (kv.second == KeyState::JUST_RELEASED) kv.second = KeyState::RELEASED;
            else if (kv.second == KeyState::RELEASED) kv.second = KeyState::NONE;
        }

        // Reset per-frame mouse values
        mouseDeltaX = 0.0;
        mouseDeltaY = 0.0;
        scrollDeltaX = 0.0;
        scrollDeltaY = 0.0;
    }

    // --- Keyboard queries ---
    static bool KeyTapped(int key) { return get(keys, key) == KeyState::PRESSED; }
    static bool KeyPressed(int key) { return get(keys, key) == KeyState::PRESSED || get(keys, key) == KeyState::HELD; }
    static bool KeyHeld(int key) { return get(keys, key) == KeyState::HELD; }
    static bool KeyReleased(int key) { return get(keys, key) == KeyState::RELEASED; }

    // --- Mouse button queries ---
    static bool MousePressed(int btn) { return get(buttons, btn) == KeyState::PRESSED || get(buttons, btn) == KeyState::HELD; }
    static bool MouseHeld(int btn) { return get(buttons, btn) == KeyState::HELD; }
    static bool MouseReleased(int btn) { return get(buttons, btn) == KeyState::RELEASED; }

    // --- Mouse movement ---
    static void GetMousePosition(double& x, double& y) {
        x = mouseX;
        y = mouseY;
    }

    static void GetMouseDelta(double& dx, double& dy) {
        dx = mouseDeltaX;
        dy = mouseDeltaY;
    }

    // --- Scroll wheel ---
    static void GetScrollDelta(double& dx, double& dy) {
        dx = scrollDeltaX;
        dy = scrollDeltaY;
    }

    static void BlockInputForUI(bool block) {
        inputBlockedForUI = block;
    }

    static bool IsInputBlockedForUI() {
        return inputBlockedForUI;
    }

    static void HideCursor(bool hide) {
        glfwSetInputMode(
            window,
            GLFW_CURSOR,
            hide ? GLFW_CURSOR_HIDDEN : GLFW_CURSOR_NORMAL
        );
    }

    static int CharToKeycode(char c) {
        if (std::isdigit(static_cast<unsigned char>(c)))
            return GLFW_KEY_0 + (c - '0');

        if (std::isalpha(static_cast<unsigned char>(c))) {
            char upper = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            return GLFW_KEY_A + (upper - 'A');
        }

        switch (c) {
        case ' ': return GLFW_KEY_SPACE;
        case '-': return GLFW_KEY_MINUS;
        case '=': return GLFW_KEY_EQUAL;
        case '[': return GLFW_KEY_LEFT_BRACKET;
        case ']': return GLFW_KEY_RIGHT_BRACKET;
        case ';': return GLFW_KEY_SEMICOLON;
        case '\'':return GLFW_KEY_APOSTROPHE;
        case ',': return GLFW_KEY_COMMA;
        case '.': return GLFW_KEY_PERIOD;
        case '/': return GLFW_KEY_SLASH;
        case '\\':return GLFW_KEY_BACKSLASH;
        case '`': return GLFW_KEY_GRAVE_ACCENT;
        default:  return GLFW_KEY_UNKNOWN;
        }
    }

private:
    static inline GLFWwindow* window = nullptr;

    static inline std::unordered_map<int, KeyState> keys;
    static inline std::unordered_map<int, KeyState> buttons;

    static inline bool inputBlockedForUI = false;

    // Mouse state
    static inline double mouseX = 0.0;
    static inline double mouseY = 0.0;
    static inline double lastMouseX = 0.0;
    static inline double lastMouseY = 0.0;
    static inline double mouseDeltaX = 0.0;
    static inline double mouseDeltaY = 0.0;
    static inline double scrollDeltaX = 0.0;
    static inline double scrollDeltaY = 0.0;

    static void keyCallback(GLFWwindow*, int key, int, int action, int) {
        if (action == GLFW_PRESS)   keys[key] = KeyState::JUST_PRESSED;
        if (action == GLFW_RELEASE) keys[key] = KeyState::JUST_RELEASED;
    }

#include <iostream>

    static void mouseButtonCallback(GLFWwindow*, int button, int action, int) {
        if (action == GLFW_PRESS) {
            buttons[button] = KeyState::JUST_PRESSED;
        }
        else if (action == GLFW_RELEASE) {
            buttons[button] = KeyState::JUST_RELEASED;
        }
    }


    static void cursorPosCallback(GLFWwindow*, double x, double y) {
        mouseDeltaX += (x - lastMouseX);
        mouseDeltaY += (y - lastMouseY);

        lastMouseX = x;
        lastMouseY = y;
        mouseX = x;
        mouseY = y;
    }

    static void scrollCallback(GLFWwindow*, double xoffset, double yoffset) {
        scrollDeltaX += xoffset;
        scrollDeltaY += yoffset;
    }

    template<typename Map>
    static KeyState get(const Map& m, int code) {
        auto it = m.find(code);
        return it == m.end() ? KeyState::NONE : it->second;
    }
};

#endif // INPUT_HPP
