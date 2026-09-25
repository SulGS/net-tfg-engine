#ifndef INPUT_HPP
#define INPUT_HPP

#include <GLFW/glfw3.h>
#include <unordered_map>
#include <unordered_set>
#include <cctype>

// Frame loop: glfwPollEvents(); Input::Update(); then systems query Input (Update() first also works, one frame later).
// KeyTapped/MouseTapped: one frame, the press. KeyPressed/MousePressed: whole time down. KeyHeld/MouseHeld: down,
// except the first frame. KeyReleased/MouseReleased: one frame, the release.
class Input {
public:
    enum class KeyState { NONE, JUST_PRESSED, PRESSED, HELD, JUST_RELEASED, RELEASED };

    static constexpr int ArrowLeft = GLFW_KEY_LEFT;
    static constexpr int ArrowRight = GLFW_KEY_RIGHT;
    static constexpr int ArrowUp = GLFW_KEY_UP;
    static constexpr int ArrowDown = GLFW_KEY_DOWN;
    static constexpr int Enter = GLFW_KEY_ENTER;
    static constexpr int Escape = GLFW_KEY_ESCAPE;
    static constexpr int Tab = GLFW_KEY_TAB;
    static constexpr int Backspace = GLFW_KEY_BACKSPACE;
    static constexpr int Shift = GLFW_KEY_LEFT_SHIFT;
    static constexpr int Ctrl = GLFW_KEY_LEFT_CONTROL;
    static constexpr int Alt = GLFW_KEY_LEFT_ALT;

    // Call once after you create the GLFW window
    static void Init(GLFWwindow* win) {
        window = win;

        glfwSetKeyCallback(window, keyCallback);
        glfwSetMouseButtonCallback(window, mouseButtonCallback);
        glfwSetCursorPosCallback(window, cursorPosCallback);
        glfwSetScrollCallback(window, scrollCallback);

        glfwGetCursorPos(window, &mouseX, &mouseY);
        lastMouseX = mouseX;
        lastMouseY = mouseY;
    }

    // Call once per frame, after glfwPollEvents()
    static void Update() {
        ageStates(keys, pendingKeyRelease);
        ageStates(buttons, pendingButtonRelease);

        // Publish what the callbacks accumulated since the previous Update().
        // The deltas used to be zeroed here, which meant that anything reading
        // them after Update() always got 0.
        mouseDeltaX = pendingMouseDeltaX;
        mouseDeltaY = pendingMouseDeltaY;
        scrollDeltaX = pendingScrollDeltaX;
        scrollDeltaY = pendingScrollDeltaY;

        pendingMouseDeltaX = 0.0;
        pendingMouseDeltaY = 0.0;
        pendingScrollDeltaX = 0.0;
        pendingScrollDeltaY = 0.0;
    }

    static bool KeyTapped(int key) { return get(keys, key) == KeyState::PRESSED; }
    static bool KeyPressed(int key) { return get(keys, key) == KeyState::PRESSED || get(keys, key) == KeyState::HELD; }
    static bool KeyHeld(int key) { return get(keys, key) == KeyState::HELD; }
    static bool KeyReleased(int key) { return get(keys, key) == KeyState::RELEASED; }

    // MouseTapped is the press edge; MousePressed stays true while held
    static bool MouseTapped(int btn) { return get(buttons, btn) == KeyState::PRESSED; }
    static bool MousePressed(int btn) { return get(buttons, btn) == KeyState::PRESSED || get(buttons, btn) == KeyState::HELD; }
    static bool MouseHeld(int btn) { return get(buttons, btn) == KeyState::HELD; }
    static bool MouseReleased(int btn) { return get(buttons, btn) == KeyState::RELEASED; }

    static void GetMousePosition(double& x, double& y) {
        x = mouseX;
        y = mouseY;
    }

    static void GetMouseDelta(double& dx, double& dy) {
        dx = mouseDeltaX;
        dy = mouseDeltaY;
    }

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

    // Forget every pressed key/button. Useful when the window loses focus or
    // when you open a menu, so nothing stays stuck as held.
    static void ClearStates() {
        keys.clear();
        buttons.clear();
        pendingKeyRelease.clear();
        pendingButtonRelease.clear();
        mouseDeltaX = mouseDeltaY = 0.0;
        scrollDeltaX = scrollDeltaY = 0.0;
        pendingMouseDeltaX = pendingMouseDeltaY = 0.0;
        pendingScrollDeltaX = pendingScrollDeltaY = 0.0;
    }

    static void HideCursor(bool hide) {
        glfwSetInputMode(
            window,
            GLFW_CURSOR,
            hide ? GLFW_CURSOR_HIDDEN : GLFW_CURSOR_NORMAL
        );

        // Keep the delta from exploding on the first frame after the change
        glfwGetCursorPos(window, &mouseX, &mouseY);
        lastMouseX = mouseX;
        lastMouseY = mouseY;
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

    // Codes released in the very same frame they were pressed. The release is
    // deferred so a fast click is never swallowed.
    static inline std::unordered_set<int> pendingKeyRelease;
    static inline std::unordered_set<int> pendingButtonRelease;

    static inline bool inputBlockedForUI = false;

    // Mouse state
    static inline double mouseX = 0.0;
    static inline double mouseY = 0.0;
    static inline double lastMouseX = 0.0;
    static inline double lastMouseY = 0.0;

    // Published to the rest of the engine by Update()
    static inline double mouseDeltaX = 0.0;
    static inline double mouseDeltaY = 0.0;
    static inline double scrollDeltaX = 0.0;
    static inline double scrollDeltaY = 0.0;

    // Written by the GLFW callbacks
    static inline double pendingMouseDeltaX = 0.0;
    static inline double pendingMouseDeltaY = 0.0;
    static inline double pendingScrollDeltaX = 0.0;
    static inline double pendingScrollDeltaY = 0.0;

    static void ageStates(std::unordered_map<int, KeyState>& states,
        std::unordered_set<int>& pendingRelease) {
        for (auto& kv : states) {
            switch (kv.second) {
            case KeyState::JUST_PRESSED:
                kv.second = KeyState::PRESSED;
                break;

            case KeyState::PRESSED:
                // PRESSED has now been visible for one full frame, so a release
                // that arrived too early can finally be applied.
                if (pendingRelease.erase(kv.first) > 0) {
                    kv.second = KeyState::RELEASED;
                }
                else {
                    kv.second = KeyState::HELD;
                }
                break;

            case KeyState::JUST_RELEASED:
                kv.second = KeyState::RELEASED;
                break;

            case KeyState::RELEASED:
                kv.second = KeyState::NONE;
                break;

            default:
                break;
            }
        }
    }

    static void press(std::unordered_map<int, KeyState>& states,
        std::unordered_set<int>& pendingRelease, int code) {
        states[code] = KeyState::JUST_PRESSED;
        pendingRelease.erase(code);
    }

    static void release(std::unordered_map<int, KeyState>& states,
        std::unordered_set<int>& pendingRelease, int code) {
        // Releasing before the press was ever reported would hide the click
        // entirely, so hold it back until the next Update().
        if (get(states, code) == KeyState::JUST_PRESSED) {
            pendingRelease.insert(code);
            return;
        }
        states[code] = KeyState::JUST_RELEASED;
    }

    static void keyCallback(GLFWwindow*, int key, int, int action, int) {
        if (key == GLFW_KEY_UNKNOWN) return;
        if (action == GLFW_PRESS)   press(keys, pendingKeyRelease, key);
        if (action == GLFW_RELEASE) release(keys, pendingKeyRelease, key);
        // GLFW_REPEAT is ignored on purpose: HELD already covers it
    }

    static void mouseButtonCallback(GLFWwindow*, int button, int action, int) {
        if (action == GLFW_PRESS)   press(buttons, pendingButtonRelease, button);
        if (action == GLFW_RELEASE) release(buttons, pendingButtonRelease, button);
    }

    static void cursorPosCallback(GLFWwindow*, double x, double y) {
        pendingMouseDeltaX += (x - lastMouseX);
        pendingMouseDeltaY += (y - lastMouseY);

        lastMouseX = x;
        lastMouseY = y;
        mouseX = x;
        mouseY = y;
    }

    static void scrollCallback(GLFWwindow*, double xoffset, double yoffset) {
        pendingScrollDeltaX += xoffset;
        pendingScrollDeltaY += yoffset;
    }

    template<typename Map>
    static KeyState get(const Map& m, int code) {
        auto it = m.find(code);
        return it == m.end() ? KeyState::NONE : it->second;
    }
};

#endif // INPUT_HPP