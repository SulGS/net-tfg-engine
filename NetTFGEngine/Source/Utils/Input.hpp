#ifndef INPUT_HPP
#define INPUT_HPP

#include <GLFW/glfw3.h>
#include <unordered_map>

class Input {
public:
    enum class KeyState { NONE, PRESSED, HELD, RELEASED };

    // Call once after you create the GLFW window
    static void Init(GLFWwindow* win) {
        window = win;
        glfwSetKeyCallback(window, keyCallback);
        glfwSetMouseButtonCallback(window, mouseButtonCallback);
    }

    // Call once per frame, after glfwPollEvents()
    static void Update() {
        for (auto &kv : keys) {
            if (kv.second == KeyState::PRESSED)   kv.second = KeyState::HELD;
            else if (kv.second == KeyState::RELEASED) kv.second = KeyState::NONE;
        }
        for (auto &kv : buttons) {
            if (kv.second == KeyState::PRESSED)   kv.second = KeyState::HELD;
            else if (kv.second == KeyState::RELEASED) kv.second = KeyState::NONE;
        }
    }

    // Query functions
    static bool KeyTapped(int key)   { return get(keys, key)   == KeyState::PRESSED; }
    static bool KeyPressed(int key)   { return get(keys, key)   == KeyState::PRESSED || get(keys, key)   == KeyState::HELD; }
    static bool KeyHeld(int key)      { return get(keys, key)   == KeyState::HELD; }
    static bool KeyReleased(int key)  { return get(keys, key)   == KeyState::RELEASED; }

    static bool MousePressed(int btn) { return get(buttons, btn)== KeyState::PRESSED; }
    static bool MouseHeld(int btn)    { return get(buttons, btn)== KeyState::HELD; }
    static bool MouseReleased(int btn){ return get(buttons, btn)== KeyState::RELEASED; }

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

    static void keyCallback(GLFWwindow*, int key, int scancode, int action, int mods) {
        if (action == GLFW_PRESS)   keys[key] = KeyState::PRESSED;
        if (action == GLFW_RELEASE) keys[key] = KeyState::RELEASED;
        //std::cout << "Key event: key=" << key << " action=" << action << "\n";
    }

    static void mouseButtonCallback(GLFWwindow*, int button, int action, int mods) {
        if (action == GLFW_PRESS)   buttons[button] = KeyState::PRESSED;
        if (action == GLFW_RELEASE) buttons[button] = KeyState::RELEASED;
    }

    template<typename Map>
    static KeyState get(const Map& m, int code) {
        auto it = m.find(code);
        return it == m.end() ? KeyState::NONE : it->second;
    }
};

#endif // INPUT_HPP
