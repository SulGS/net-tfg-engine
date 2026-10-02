#ifndef INPUT_HPP
#define INPUT_HPP

#include <GLFW/glfw3.h>
#include <unordered_map>
#include <unordered_set>
#include <cctype>
#include <cmath>

// Frame loop: glfwPollEvents(); Input::Update(); then systems query Input (Update() first also works, one frame later).
// KeyTapped/MouseTapped: one frame, the press. KeyPressed/MousePressed: whole time down. KeyHeld/MouseHeld: down,
// except the first frame. KeyReleased/MouseReleased: one frame, the release.
//
// Gamepad: the first connected joystick GLFW recognises as a gamepad (standard layout: A/B/X/Y, bumpers, triggers,
// sticks), polled in Update(). Buttons follow the same Tapped/Pressed/Held/Released states as keys; axes are raw
// (sticks -1..1, triggers -1 at rest .. 1). Everything here runs on the render thread (the one polling GLFW events);
// the game tick reads input through InputMap's snapshot, never from here.
//
// Raw device state. Game code should ask InputMap for actions ("shoot", "UI accept") instead of naming keys.
class Input {
public:
    enum class KeyState { NONE, JUST_PRESSED, PRESSED, HELD, JUST_RELEASED, RELEASED };

    // Device family that produced the latest input. Drives the mouse cursor (hidden while the gamepad is in use) and
    // the UI's focus highlight.
    enum class Device { KeyboardMouse, Gamepad };

    // A stick counts as "used" (for LastDevice / AnyGamepadAxisPushed) past this, whatever the configured deadzone.
    static constexpr float kAxisActivity = 0.5f;

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
        glfwSetWindowFocusCallback(window, windowFocusCallback);

        glfwGetCursorPos(window, &mouseX, &mouseY);
        lastMouseX = mouseX;
        lastMouseY = mouseY;
    }

    // Call once per frame, after glfwPollEvents()
    static void Update() {
        // Before aging, like a key press from the callbacks would be: a button that went down this poll is PRESSED
        // (Tapped) on the next frame, the same latency as the keyboard.
        pollGamepad();

        ageStates(keys, pendingKeyRelease);
        ageStates(buttons, pendingButtonRelease);
        ageStates(padButtons, pendingPadRelease);

        // Real mouse movement this frame. A few pixels of drift (a bumped desk) shouldn't leave gamepad mode.
        if (std::abs(pendingMouseDeltaX) + std::abs(pendingMouseDeltaY) > 4.0)
            lastDevice = Device::KeyboardMouse;

        // The pointer is useless while playing with the gamepad; any real mouse use brings it back.
        const bool wantHidden = (lastDevice == Device::Gamepad);
        if (window && wantHidden != cursorHiddenForGamepad) {
            cursorHiddenForGamepad = wantHidden;
            HideCursor(wantHidden);
        }

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

    // Gamepad buttons (GLFW_GAMEPAD_BUTTON_*), same states as keys. All false with no gamepad connected.
    static bool GamepadTapped(int btn) { return get(padButtons, btn) == KeyState::PRESSED; }
    static bool GamepadPressed(int btn) { return get(padButtons, btn) == KeyState::PRESSED || get(padButtons, btn) == KeyState::HELD; }
    static bool GamepadHeld(int btn) { return get(padButtons, btn) == KeyState::HELD; }
    static bool GamepadReleased(int btn) { return get(padButtons, btn) == KeyState::RELEASED; }

    // Raw axis (GLFW_GAMEPAD_AXIS_*): sticks -1..1 (+y is down), triggers -1 (released) .. 1. 0 / -1 when disconnected.
    static float GamepadAxis(int axis) {
        if (axis < 0 || axis > GLFW_GAMEPAD_AXIS_LAST) return 0.0f;
        return padAxes[axis];
    }
    static bool IsTriggerAxis(int axis) {
        return axis == GLFW_GAMEPAD_AXIS_LEFT_TRIGGER || axis == GLFW_GAMEPAD_AXIS_RIGHT_TRIGGER;
    }
    // Axis as a 0..1 push in one direction (dir +1 / -1). Triggers only go "+": 0 released .. 1 fully pressed.
    static float GamepadAxisPush(int axis, int dir) {
        const float v = GamepadAxis(axis);
        if (IsTriggerAxis(axis)) return dir > 0 ? (v + 1.0f) * 0.5f : 0.0f;
        const float p = v * static_cast<float>(dir);
        return p > 0.0f ? p : 0.0f;
    }

    static bool IsGamepadConnected() { return padJoystick >= 0; }
    static const char* GetGamepadName() {
        return padJoystick >= 0 ? glfwGetGamepadName(padJoystick) : nullptr;
    }

    static Device LastDevice() { return lastDevice; }

    // First key / gamepad button / stick direction that went down this frame (the Tapped edge), for rebinding.
    // False if none did. Axes report the direction pushed past kAxisActivity (triggers: dir is always +1).
    static bool AnyKeyTapped(int& keyOut) {
        for (const auto& kv : keys)
            if (kv.second == KeyState::PRESSED) { keyOut = kv.first; return true; }
        return false;
    }
    static bool AnyGamepadButtonTapped(int& buttonOut) {
        for (const auto& kv : padButtons)
            if (kv.second == KeyState::PRESSED) { buttonOut = kv.first; return true; }
        return false;
    }
    static bool AnyGamepadAxisPushed(int& axisOut, int& dirOut) {
        for (int a = 0; a <= GLFW_GAMEPAD_AXIS_LAST; ++a) {
            for (int dir : { 1, -1 }) {
                const bool now = GamepadAxisPush(a, dir) > kAxisActivity;
                const bool before = axisPushedLastPoll[a][dir > 0 ? 0 : 1];
                if (now && !before) { axisOut = a; dirOut = dir; return true; }
            }
        }
        return false;
    }

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
        padButtons.clear();
        pendingKeyRelease.clear();
        pendingButtonRelease.clear();
        pendingPadRelease.clear();
        // Held buttons were just forgotten: until they're released, pollGamepad() must not report them as new presses.
        ignorePadUntilReleased = true;
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

    // Gamepad
    static inline std::unordered_map<int, KeyState> padButtons;
    static inline std::unordered_set<int> pendingPadRelease;
    static inline int padJoystick = -1;                                  // GLFW_JOYSTICK_* in use, -1 = none
    static inline unsigned char padRawButtons[GLFW_GAMEPAD_BUTTON_LAST + 1] = {};
    static inline float padAxes[GLFW_GAMEPAD_AXIS_LAST + 1] = { 0, 0, 0, 0, -1, -1 };
    static inline bool axisPushedLastPoll[GLFW_GAMEPAD_AXIS_LAST + 1][2] = {}; // [axis][0 = +, 1 = -]
    static inline bool ignorePadUntilReleased = false;

    static inline Device lastDevice = Device::KeyboardMouse;
    static inline bool cursorHiddenForGamepad = false;

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
        if (action == GLFW_PRESS) {
            press(keys, pendingKeyRelease, key);
            lastDevice = Device::KeyboardMouse;
        }
        if (action == GLFW_RELEASE) release(keys, pendingKeyRelease, key);
        // GLFW_REPEAT is ignored on purpose: HELD already covers it
    }

    static void mouseButtonCallback(GLFWwindow*, int button, int action, int) {
        if (action == GLFW_PRESS) {
            press(buttons, pendingButtonRelease, button);
            lastDevice = Device::KeyboardMouse;
        }
        if (action == GLFW_RELEASE) release(buttons, pendingButtonRelease, button);
    }

    // Alt+Tab while holding a key never delivers its release: forget everything so nothing stays stuck down.
    static void windowFocusCallback(GLFWwindow*, int focused) {
        if (!focused) ClearStates();
    }

    // First connected gamepad (re-searched each frame while none is, so hot-plugging just works). Button edges go
    // through press()/release() like the key callbacks.
    static void pollGamepad() {
        if (padJoystick >= 0 && !glfwJoystickIsGamepad(padJoystick))
            padJoystick = -1;
        if (padJoystick < 0) {
            for (int jid = GLFW_JOYSTICK_1; jid <= GLFW_JOYSTICK_LAST; ++jid) {
                if (glfwJoystickIsGamepad(jid)) { padJoystick = jid; break; }
            }
        }

        // Pushed state as of the previous poll, for AnyGamepadAxisPushed's edge.
        for (int a = 0; a <= GLFW_GAMEPAD_AXIS_LAST; ++a) {
            axisPushedLastPoll[a][0] = GamepadAxisPush(a, 1) > kAxisActivity;
            axisPushedLastPoll[a][1] = GamepadAxisPush(a, -1) > kAxisActivity;
        }

        GLFWgamepadstate state{};
        if (padJoystick < 0 || !glfwGetGamepadState(padJoystick, &state)) {
            // Disconnected: release whatever was down, rest the axes.
            state = GLFWgamepadstate{};
            state.axes[GLFW_GAMEPAD_AXIS_LEFT_TRIGGER] = -1.0f;
            state.axes[GLFW_GAMEPAD_AXIS_RIGHT_TRIGGER] = -1.0f;
        }

        bool anyDown = false;
        for (int b = 0; b <= GLFW_GAMEPAD_BUTTON_LAST; ++b) {
            const bool down = state.buttons[b] == GLFW_PRESS;
            anyDown |= down;
            const bool wasDown = padRawButtons[b] == GLFW_PRESS;
            padRawButtons[b] = state.buttons[b];
            if (ignorePadUntilReleased) continue;
            if (down && !wasDown) {
                press(padButtons, pendingPadRelease, b);
                lastDevice = Device::Gamepad;
            }
            if (!down && wasDown) release(padButtons, pendingPadRelease, b);
        }
        if (!anyDown) ignorePadUntilReleased = false;

        for (int a = 0; a <= GLFW_GAMEPAD_AXIS_LAST; ++a) {
            padAxes[a] = state.axes[a];
            if (GamepadAxisPush(a, 1) > kAxisActivity || GamepadAxisPush(a, -1) > kAxisActivity)
                lastDevice = Device::Gamepad;
        }
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