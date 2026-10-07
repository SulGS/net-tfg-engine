#ifndef INPUTMAP_HPP
#define INPUTMAP_HPP

#include "Utils/Input.hpp"
#include "Utils/UserDataPath.hpp"

#include <array>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

// Actions on top of Input: code asks for "shoot" or "UI accept", and InputMap resolves it through the bindings the
// player chose. Every action has one rebindable binding per slot (keyboard, gamepad) plus optional fixed extras (e.g.
// the D-pad next to the stick for UI navigation) that are never shown nor saved.
//
// Threads: Update() and the Pressed/Tapped/... queries are for the render thread (right after Input::Update(), which
// is where both live). The game tick, on its own thread, only calls ConsumeTickInput(), which reads a snapshot taken
// under a mutex, so it never touches Input's maps while GLFW callbacks write them.
//
// Ids: 0..kFirstGameAction-1 are the engine's UI actions (UIAction below), registered by InputMap itself; the game
// registers its own from kFirstGameAction up, before the engine starts.

enum class BindingType : uint8_t { None = 0, Key, GamepadButton, GamepadAxis };

struct InputBinding {
    BindingType type = BindingType::None;
    int code = 0;   // GLFW_KEY_*, GLFW_GAMEPAD_BUTTON_* or GLFW_GAMEPAD_AXIS_*
    int dir = 1;    // axes only: +1 / -1 (triggers: always +1)

    static InputBinding Key(int key) { return { BindingType::Key, key, 1 }; }
    static InputBinding PadButton(int button) { return { BindingType::GamepadButton, button, 1 }; }
    static InputBinding PadAxis(int axis, int dir) { return { BindingType::GamepadAxis, axis, dir >= 0 ? 1 : -1 }; }

    bool IsValid() const { return type != BindingType::None; }

    bool operator==(const InputBinding& o) const {
        if (type != o.type) return false;
        if (type == BindingType::None) return true;
        return code == o.code && (type != BindingType::GamepadAxis || dir == o.dir);
    }
    bool operator!=(const InputBinding& o) const { return !(*this == o); }

    // Config file form: "none", "key:87", "button:0", "axis:5:+".
    std::string Serialize() const {
        switch (type) {
        case BindingType::Key:           return "key:" + std::to_string(code);
        case BindingType::GamepadButton: return "button:" + std::to_string(code);
        case BindingType::GamepadAxis:   return "axis:" + std::to_string(code) + (dir > 0 ? ":+" : ":-");
        default:                         return "none";
        }
    }

    // Anything malformed or out of range parses as None (the caller keeps the default then).
    static bool Parse(const std::string& s, InputBinding& out) {
        if (s == "none") { out = InputBinding{}; return true; }
        const size_t colon = s.find(':');
        if (colon == std::string::npos) return false;
        const std::string kind = s.substr(0, colon);
        const std::string rest = s.substr(colon + 1);
        try {
            if (kind == "key") {
                const int k = std::stoi(rest);
                if (k < GLFW_KEY_SPACE || k > GLFW_KEY_LAST) return false;
                out = Key(k);
                return true;
            }
            if (kind == "button") {
                const int b = std::stoi(rest);
                if (b < 0 || b > GLFW_GAMEPAD_BUTTON_LAST) return false;
                out = PadButton(b);
                return true;
            }
            if (kind == "axis") {
                const size_t colon2 = rest.find(':');
                if (colon2 == std::string::npos) return false;
                const int a = std::stoi(rest.substr(0, colon2));
                const std::string d = rest.substr(colon2 + 1);
                if (a < 0 || a > GLFW_GAMEPAD_AXIS_LAST || (d != "+" && d != "-")) return false;
                out = PadAxis(a, d == "+" ? 1 : -1);
                return true;
            }
        }
        catch (...) {
        }
        return false;
    }

    // Text for the settings UI. Keys use the current keyboard layout for printable characters (glfwGetKeyName, render
    // thread only); gamepad names follow the Xbox layout GLFW maps every gamepad to.
    std::string DisplayName() const {
        switch (type) {
        case BindingType::Key:           return KeyName(code);
        case BindingType::GamepadButton: return PadButtonName(code);
        case BindingType::GamepadAxis:   return PadAxisName(code, dir);
        default:                         return "Sin asignar";
        }
    }

    static std::string KeyName(int key) {
        switch (key) {
        case GLFW_KEY_SPACE:         return "Espacio";
        case GLFW_KEY_ESCAPE:        return "Esc";
        case GLFW_KEY_ENTER:         return "Intro";
        case GLFW_KEY_KP_ENTER:      return "Intro (teclado num.)";
        case GLFW_KEY_TAB:           return "Tab";
        case GLFW_KEY_BACKSPACE:     return "Retroceso";
        case GLFW_KEY_INSERT:        return "Insert";
        case GLFW_KEY_DELETE:        return "Supr";
        case GLFW_KEY_RIGHT:         return "Flecha derecha";
        case GLFW_KEY_LEFT:          return "Flecha izquierda";
        case GLFW_KEY_DOWN:          return "Flecha abajo";
        case GLFW_KEY_UP:            return "Flecha arriba";
        case GLFW_KEY_PAGE_UP:       return "Re Pág";
        case GLFW_KEY_PAGE_DOWN:     return "Av Pág";
        case GLFW_KEY_HOME:          return "Inicio";
        case GLFW_KEY_END:           return "Fin";
        case GLFW_KEY_CAPS_LOCK:     return "Bloq Mayús";
        case GLFW_KEY_LEFT_SHIFT:    return "Mayús izq.";
        case GLFW_KEY_RIGHT_SHIFT:   return "Mayús der.";
        case GLFW_KEY_LEFT_CONTROL:  return "Ctrl izq.";
        case GLFW_KEY_RIGHT_CONTROL: return "Ctrl der.";
        case GLFW_KEY_LEFT_ALT:      return "Alt";
        case GLFW_KEY_RIGHT_ALT:     return "Alt Gr";
        default: break;
        }
        if (key >= GLFW_KEY_F1 && key <= GLFW_KEY_F25) return "F" + std::to_string(key - GLFW_KEY_F1 + 1);
        if (key >= GLFW_KEY_KP_0 && key <= GLFW_KEY_KP_9) return "Num " + std::to_string(key - GLFW_KEY_KP_0);

        if (const char* name = glfwGetKeyName(key, 0)) {
            std::string s(name);
            if (s.size() == 1) s[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(s[0])));
            return s;
        }
        return "Tecla " + std::to_string(key);
    }

    static std::string PadButtonName(int button) {
        switch (button) {
        case GLFW_GAMEPAD_BUTTON_A:            return "A";
        case GLFW_GAMEPAD_BUTTON_B:            return "B";
        case GLFW_GAMEPAD_BUTTON_X:            return "X";
        case GLFW_GAMEPAD_BUTTON_Y:            return "Y";
        case GLFW_GAMEPAD_BUTTON_LEFT_BUMPER:  return "LB";
        case GLFW_GAMEPAD_BUTTON_RIGHT_BUMPER: return "RB";
        case GLFW_GAMEPAD_BUTTON_BACK:         return "Back";
        case GLFW_GAMEPAD_BUTTON_START:        return "Start";
        case GLFW_GAMEPAD_BUTTON_GUIDE:        return "Guide";
        case GLFW_GAMEPAD_BUTTON_LEFT_THUMB:   return "Stick izq. (pulsar)";
        case GLFW_GAMEPAD_BUTTON_RIGHT_THUMB:  return "Stick der. (pulsar)";
        case GLFW_GAMEPAD_BUTTON_DPAD_UP:      return "Cruceta arriba";
        case GLFW_GAMEPAD_BUTTON_DPAD_RIGHT:   return "Cruceta derecha";
        case GLFW_GAMEPAD_BUTTON_DPAD_DOWN:    return "Cruceta abajo";
        case GLFW_GAMEPAD_BUTTON_DPAD_LEFT:    return "Cruceta izquierda";
        default:                               return "Botón " + std::to_string(button);
        }
    }

    static std::string PadAxisName(int axis, int dir) {
        switch (axis) {
        case GLFW_GAMEPAD_AXIS_LEFT_X:        return dir > 0 ? "Stick izq. derecha" : "Stick izq. izquierda";
        case GLFW_GAMEPAD_AXIS_LEFT_Y:        return dir > 0 ? "Stick izq. abajo" : "Stick izq. arriba";
        case GLFW_GAMEPAD_AXIS_RIGHT_X:       return dir > 0 ? "Stick der. derecha" : "Stick der. izquierda";
        case GLFW_GAMEPAD_AXIS_RIGHT_Y:       return dir > 0 ? "Stick der. abajo" : "Stick der. arriba";
        case GLFW_GAMEPAD_AXIS_LEFT_TRIGGER:  return "LT";
        case GLFW_GAMEPAD_AXIS_RIGHT_TRIGGER: return "RT";
        default:                              return "Eje " + std::to_string(axis);
        }
    }
};

enum class BindingSlot : int { Keyboard = 0, Gamepad = 1 };

// Engine UI actions. Fixed (not rebindable): the UI must stay usable whatever the game's bindings are.
namespace UIAction {
    enum : int {
        Up = 0,
        Down,
        Left,
        Right,
        Accept,
        Back,
        PrevTab,
        NextTab,
        ScrollUp,     // scroll views (UIScrollView): right stick, analog (Value() is the speed)
        ScrollDown,
        Count
    };
}

class InputMap {
public:
    static constexpr int kFirstGameAction = 16;
    static constexpr int kMaxActions = 64;
    static constexpr int kSlotCount = 2;

    // What the game tick sees: actions down now, plus any tapped since the previous tick (so a press shorter than a
    // tick, or between two ticks at a high frame rate, still reaches the simulation once).
    struct TickInput {
        uint64_t bits = 0;
        std::array<float, kMaxActions> values{};
        bool Down(int id) const { return id >= 0 && id < kMaxActions && (bits >> id) & 1ull; }
        // 0..1 intensity (see InputMap::Value); 1 for keys/buttons and for a tap already released.
        float Value(int id) const { return (id >= 0 && id < kMaxActions) ? values[id] : 0.0f; }
    };

    static InputMap& Get() {
        static InputMap instance;
        return instance;
    }

    // --- Registration (main thread, before the engine starts) ------------------------------------------------------

    // name: key in input_bindings.cfg (stable, no spaces). label: shown in the settings. gameplay: blocked while the
    // UI has the input (text field focused, pause menu open...). Only rebindable actions are listed and saved.
    void RegisterAction(int id, const std::string& name, const std::string& label,
        InputBinding keyboard, InputBinding gamepad, bool gameplay, bool rebindable = true) {
        if (id < 0 || id >= kMaxActions) return;
        Action& a = actions[id];
        a.registered = true;
        a.name = name;
        a.label = label;
        a.gameplay = gameplay;
        a.rebindable = rebindable;
        a.defaults[0] = a.bindings[0] = keyboard;
        a.defaults[1] = a.bindings[1] = gamepad;
        a.extras.clear();

        if (rebindable && std::find(rebindableOrder.begin(), rebindableOrder.end(), id) == rebindableOrder.end())
            rebindableOrder.push_back(id);

        EnsureLoaded();
        ApplyOverride(id);
    }

    // Extra binding that always works alongside the configurable ones (not shown, not saved).
    void AddFixedBinding(int id, InputBinding binding) {
        if (id < 0 || id >= kMaxActions || !actions[id].registered) return;
        actions[id].extras.push_back(binding);
    }

    // Rebindable actions, in registration order (the settings list).
    const std::vector<int>& RebindableActions() const { return rebindableOrder; }

    std::string GetLabel(int id) const { return Valid(id) ? actions[id].label : std::string(); }

    InputBinding GetBinding(int id, BindingSlot slot) const {
        return Valid(id) ? actions[id].bindings[static_cast<int>(slot)] : InputBinding{};
    }

    // Returns the label of the action it was swapped with, if the binding was already used by another rebindable
    // action in the same slot (that one gets this action's previous binding), or "" if there was no conflict.
    std::string SetBinding(int id, BindingSlot slot, InputBinding binding) {
        if (!Valid(id)) return {};
        const int s = static_cast<int>(slot);
        const InputBinding previous = actions[id].bindings[s];
        std::string swappedWith;
        if (binding.IsValid()) {
            for (int other : rebindableOrder) {
                if (other != id && actions[other].bindings[s] == binding) {
                    actions[other].bindings[s] = previous;
                    swappedWith = actions[other].label;
                }
            }
        }
        actions[id].bindings[s] = binding;
        return swappedWith;
    }

    void ResetBindingsToDefaults() {
        CancelCapture();
        for (Action& a : actions) {
            if (!a.registered || !a.rebindable) continue;
            a.bindings[0] = a.defaults[0];
            a.bindings[1] = a.defaults[1];
        }
        stickDeadzone = kDefaultDeadzone;
    }

    // Stick push (0..1) below which a stick binding doesn't count as pressed.
    float GetStickDeadzone() const { return stickDeadzone; }
    void SetStickDeadzone(float v) { stickDeadzone = std::clamp(v, 0.1f, 0.9f); }

    // --- Per frame (render thread) ---------------------------------------------------------------------------------

    // Gameplay actions read as released for the next Update() (call every frame while e.g. the pause menu is open).
    // One frame at a time on purpose: a scene torn down while blocking can't leave the game stuck blocked.
    void BlockGameplayThisFrame() { gameplayBlockRequested = true; }

    // Call once per frame, right after Input::Update().
    void Update() {
        EnsureLoaded();

        const auto now = std::chrono::steady_clock::now();
        float dt = hasLastUpdate ? std::chrono::duration<float>(now - lastUpdate).count() : 0.0f;
        dt = std::clamp(dt, 0.0f, 0.1f);
        lastUpdate = now;
        hasLastUpdate = true;

        const bool blockGameplay = gameplayBlockRequested || Input::IsInputBlockedForUI();
        gameplayBlockRequested = false;

        if (capturing) ProcessCapture(now);

        // See ProcessCapture: the input that ended a capture acts on nothing until released.
        if (swallowPending) {
            if (!IsBindingDown(swallowBinding, 0.5f, 0.5f)) swallowPending = false;
        }
        const bool suppressAll = capturing || swallowPending;

        uint64_t down = 0, tapped = 0, released = 0, repeated = 0;
        std::array<float, kMaxActions> strength{};
        for (int id = 0; id < kMaxActions; ++id) {
            const Action& a = actions[id];
            if (!a.registered) continue;

            if (!suppressAll && !(a.gameplay && blockGameplay)) strength[id] = ActionStrength(a);
            const bool isDown = strength[id] > 0.0f;
            const bool wasDown = (downBits >> id) & 1ull;
            const uint64_t bit = 1ull << id;

            if (isDown) down |= bit;
            if (isDown && !wasDown) {
                tapped |= bit;
                repeated |= bit;
                repeatTimers[id] = kRepeatDelay;
            }
            else if (isDown) {
                repeatTimers[id] -= dt;
                if (repeatTimers[id] <= 0.0f) {
                    repeated |= bit;
                    repeatTimers[id] += kRepeatInterval;
                }
            }
            if (!isDown && wasDown) released |= bit;
        }

        downBits = down;
        tappedBits = tapped;
        releasedBits = released;
        repeatedBits = repeated;
        values = strength;

        std::lock_guard<std::mutex> lock(tickMutex);
        tickDownBits = down;
        tickValues = strength;
        for (int id = 0; id < kMaxActions; ++id)
            if ((tapped >> id) & 1ull) lastTap[id] = now;
    }

    bool Pressed(int id) const { return Bit(downBits, id); }
    bool Tapped(int id) const { return Bit(tappedBits, id); }
    bool Released(int id) const { return Bit(releasedBits, id); }
    // Tapped, then again every kRepeatInterval while held (after kRepeatDelay): menu navigation.
    bool Repeated(int id) const { return Bit(repeatedBits, id); }
    // How far it's pressed, 0..1: keys and buttons are 0 or 1; sticks and triggers rise from 0 at the deadzone (or
    // trigger threshold) to 1 at full travel. The strongest of the action's bindings. > 0 exactly when Pressed().
    float Value(int id) const { return (id >= 0 && id < kMaxActions) ? values[id] : 0.0f; }

    // --- Game tick thread --------------------------------------------------------------------------------------------

    TickInput ConsumeTickInput() {
        const auto now = std::chrono::steady_clock::now();
        std::lock_guard<std::mutex> lock(tickMutex);
        TickInput result;
        result.bits = tickDownBits;
        result.values = tickValues;
        for (int id = 0; id < kMaxActions; ++id) {
            // Taps older than kTapMemory are dropped: a press made while another scene was active (nothing consumed
            // it) must not fire on the first tick of the match.
            if (lastTap[id] > lastConsume && now - lastTap[id] < kTapMemory) {
                result.bits |= 1ull << id;
                // Already released by now: it was a quick tap, counted as a full press.
                if (result.values[id] <= 0.0f) result.values[id] = 1.0f;
            }
        }
        lastConsume = now;
        return result;
    }

    // --- Rebinding (render thread) ----------------------------------------------------------------------------------

    // Waits for the next key (Keyboard slot) or gamepad button / stick / trigger (Gamepad slot) and binds it. While
    // capturing, every action reads as released. Cancelled by Esc, the gamepad's Back button, a mouse click, the
    // other device, or after kCaptureTimeout.
    void BeginCapture(int id, BindingSlot slot) {
        if (!Valid(id) || !actions[id].rebindable) return;
        capturing = true;
        captureId = id;
        captureSlot = slot;
        captureStart = std::chrono::steady_clock::now();
    }

    void CancelCapture() { capturing = false; }

    bool IsCapturing() const { return capturing; }
    bool IsCapturing(int id, BindingSlot slot) const { return capturing && captureId == id && captureSlot == slot; }

    // Result of the last capture ("" when there is nothing new), for the settings status line.
    std::string TakeCaptureMessage() {
        std::string m;
        m.swap(captureMessage);
        return m;
    }

    // --- Persistence -------------------------------------------------------------------------------------------------

    bool Save() {
        std::ofstream f(SettingsPath());
        if (!f) return false;
        f << "# Controles. bind <accion> <kb|pad> <none | key:N | button:N | axis:N:+/->\n";
        f << "deadzone " << stickDeadzone << "\n";
        for (int id : rebindableOrder) {
            const Action& a = actions[id];
            f << "bind " << a.name << " kb " << a.bindings[0].Serialize() << "\n";
            f << "bind " << a.name << " pad " << a.bindings[1].Serialize() << "\n";
        }
        return static_cast<bool>(f);
    }

private:
    static constexpr float kDefaultDeadzone = 0.35f;
    // Low: past it the trigger is proportional (Value), so a high threshold would just waste travel.
    static constexpr float kTriggerThreshold = 0.15f;
    static constexpr float kRepeatDelay = 0.40f;
    static constexpr float kRepeatInterval = 0.12f;
    static constexpr auto kTapMemory = std::chrono::milliseconds(250);
    static constexpr auto kCaptureTimeout = std::chrono::seconds(6);

    struct Action {
        bool registered = false;
        bool gameplay = false;
        bool rebindable = false;
        std::string name;
        std::string label;
        InputBinding defaults[kSlotCount];
        InputBinding bindings[kSlotCount];
        std::vector<InputBinding> extras;
    };

    std::array<Action, kMaxActions> actions{};
    std::vector<int> rebindableOrder;
    float stickDeadzone = kDefaultDeadzone;

    // Loaded from input_bindings.cfg, applied as each action registers: name -> [slot] binding text.
    std::map<std::string, std::array<std::string, kSlotCount>> overrides;
    bool loaded = false;

    uint64_t downBits = 0, tappedBits = 0, releasedBits = 0, repeatedBits = 0;
    std::array<float, kMaxActions> values{};
    std::array<float, kMaxActions> repeatTimers{};
    std::chrono::steady_clock::time_point lastUpdate{};
    bool hasLastUpdate = false;
    bool gameplayBlockRequested = false;

    std::mutex tickMutex;
    uint64_t tickDownBits = 0;
    std::array<float, kMaxActions> tickValues{};
    std::array<std::chrono::steady_clock::time_point, kMaxActions> lastTap{};
    std::chrono::steady_clock::time_point lastConsume{};

    bool capturing = false;
    int captureId = -1;
    BindingSlot captureSlot = BindingSlot::Keyboard;
    std::chrono::steady_clock::time_point captureStart{};
    std::string captureMessage;
    bool swallowPending = false;
    InputBinding swallowBinding;

    InputMap() {
        using B = InputBinding;
        RegisterBuiltin(UIAction::Up, "ui_up", B::Key(GLFW_KEY_UP), B::PadButton(GLFW_GAMEPAD_BUTTON_DPAD_UP));
        RegisterBuiltin(UIAction::Down, "ui_down", B::Key(GLFW_KEY_DOWN), B::PadButton(GLFW_GAMEPAD_BUTTON_DPAD_DOWN));
        RegisterBuiltin(UIAction::Left, "ui_left", B::Key(GLFW_KEY_LEFT), B::PadButton(GLFW_GAMEPAD_BUTTON_DPAD_LEFT));
        RegisterBuiltin(UIAction::Right, "ui_right", B::Key(GLFW_KEY_RIGHT), B::PadButton(GLFW_GAMEPAD_BUTTON_DPAD_RIGHT));
        RegisterBuiltin(UIAction::Accept, "ui_accept", B::Key(GLFW_KEY_ENTER), B::PadButton(GLFW_GAMEPAD_BUTTON_A));
        RegisterBuiltin(UIAction::Back, "ui_back", B::Key(GLFW_KEY_ESCAPE), B::PadButton(GLFW_GAMEPAD_BUTTON_B));
        RegisterBuiltin(UIAction::PrevTab, "ui_prev_tab", B{}, B::PadButton(GLFW_GAMEPAD_BUTTON_LEFT_BUMPER));
        RegisterBuiltin(UIAction::NextTab, "ui_next_tab", B{}, B::PadButton(GLFW_GAMEPAD_BUTTON_RIGHT_BUMPER));
        RegisterBuiltin(UIAction::ScrollUp, "ui_scroll_up", B{}, B::PadAxis(GLFW_GAMEPAD_AXIS_RIGHT_Y, -1));
        RegisterBuiltin(UIAction::ScrollDown, "ui_scroll_down", B{}, B::PadAxis(GLFW_GAMEPAD_AXIS_RIGHT_Y, 1));

        // The left stick navigates too, and the keypad Enter accepts.
        AddFixedBinding(UIAction::Up, B::PadAxis(GLFW_GAMEPAD_AXIS_LEFT_Y, -1));
        AddFixedBinding(UIAction::Down, B::PadAxis(GLFW_GAMEPAD_AXIS_LEFT_Y, 1));
        AddFixedBinding(UIAction::Left, B::PadAxis(GLFW_GAMEPAD_AXIS_LEFT_X, -1));
        AddFixedBinding(UIAction::Right, B::PadAxis(GLFW_GAMEPAD_AXIS_LEFT_X, 1));
        AddFixedBinding(UIAction::Accept, B::Key(GLFW_KEY_KP_ENTER));
    }

    // Not through RegisterAction: that one loads the config file, and the constructor may run before the user data
    // path is set (UserDataPath::SetProductName). Built-ins aren't rebindable, so the file has nothing for them.
    void RegisterBuiltin(int id, const std::string& name, InputBinding keyboard, InputBinding gamepad) {
        Action& a = actions[id];
        a.registered = true;
        a.name = name;
        a.label = name;
        a.defaults[0] = a.bindings[0] = keyboard;
        a.defaults[1] = a.bindings[1] = gamepad;
    }

    bool Valid(int id) const { return id >= 0 && id < kMaxActions && actions[id].registered; }
    static bool Bit(uint64_t bits, int id) { return id >= 0 && id < kMaxActions && ((bits >> id) & 1ull); }

    static std::string SettingsPath() { return UserDataPath::File("input_bindings.cfg"); }

    // 0..1. Axes are rescaled past the threshold, so the full range stays usable whatever the deadzone.
    static float BindingStrength(const InputBinding& b, float stickThreshold, float triggerThreshold) {
        switch (b.type) {
        case BindingType::Key:
            return Input::KeyPressed(b.code) ? 1.0f : 0.0f;
        case BindingType::GamepadButton:
            return Input::GamepadPressed(b.code) ? 1.0f : 0.0f;
        case BindingType::GamepadAxis: {
            const float threshold = Input::IsTriggerAxis(b.code) ? triggerThreshold : stickThreshold;
            const float push = Input::GamepadAxisPush(b.code, b.dir);
            if (push <= threshold) return 0.0f;
            return std::min(1.0f, (push - threshold) / (1.0f - threshold));
        }
        default:
            return 0.0f;
        }
    }

    static bool IsBindingDown(const InputBinding& b, float stickThreshold, float triggerThreshold) {
        return BindingStrength(b, stickThreshold, triggerThreshold) > 0.0f;
    }

    float ActionStrength(const Action& a) const {
        float s = 0.0f;
        for (const InputBinding& b : a.bindings) s = std::max(s, BindingStrength(b, stickDeadzone, kTriggerThreshold));
        for (const InputBinding& b : a.extras)   s = std::max(s, BindingStrength(b, stickDeadzone, kTriggerThreshold));
        return s;
    }

    void ProcessCapture(std::chrono::steady_clock::time_point now) {
        // Whatever ended the capture is still held: swallow it until released, so it doesn't also act on the UI
        // (Esc closing something, the new Enter binding clicking the focused button and restarting the capture).
        auto finish = [this](const std::string& message, InputBinding endedBy) {
            capturing = false;
            captureMessage = message;
            if (endedBy.IsValid()) {
                swallowPending = true;
                swallowBinding = endedBy;
            }
        };
        const std::string cancelled = "Cambio cancelado";

        if (captureId < 0 || !Valid(captureId)) { capturing = false; return; }
        if (now - captureStart > kCaptureTimeout) { finish("Tiempo agotado, no se cambió nada", {}); return; }
        if (Input::MouseTapped(GLFW_MOUSE_BUTTON_LEFT) || Input::MouseTapped(GLFW_MOUSE_BUTTON_RIGHT)) {
            finish(cancelled, {});
            return;
        }

        InputBinding captured;
        int key = 0, button = 0, axis = 0, dir = 0;
        if (captureSlot == BindingSlot::Keyboard) {
            if (Input::AnyGamepadButtonTapped(button)) { finish(cancelled, InputBinding::PadButton(button)); return; }
            if (Input::AnyKeyTapped(key)) {
                if (key == GLFW_KEY_ESCAPE) { finish(cancelled, InputBinding::Key(key)); return; }
                captured = InputBinding::Key(key);
            }
        }
        else {
            if (Input::AnyKeyTapped(key)) { finish(cancelled, InputBinding::Key(key)); return; }
            if (Input::AnyGamepadButtonTapped(button)) {
                if (button == GLFW_GAMEPAD_BUTTON_BACK) { finish(cancelled, InputBinding::PadButton(button)); return; }
                captured = InputBinding::PadButton(button);
            }
            else if (Input::AnyGamepadAxisPushed(axis, dir)) {
                captured = InputBinding::PadAxis(axis, dir);
            }
        }
        if (!captured.IsValid()) return;

        const std::string swappedWith = SetBinding(captureId, captureSlot, captured);
        std::string message = actions[captureId].label + ": " + captured.DisplayName();
        if (!swappedWith.empty()) message += " (intercambiado con \"" + swappedWith + "\")";
        finish(message, captured);
    }

    void EnsureLoaded() {
        if (loaded) return;
        loaded = true;

        std::ifstream f(SettingsPath());
        if (!f) return;

        std::string line;
        while (std::getline(f, line)) {
            if (line.empty() || line[0] == '#') continue;
            std::istringstream ss(line);
            std::string kind;
            ss >> kind;
            if (kind == "deadzone") {
                float v = kDefaultDeadzone;
                if (ss >> v) SetStickDeadzone(v);
            }
            else if (kind == "bind") {
                std::string name, slot, value;
                if (!(ss >> name >> slot >> value)) continue;
                if (slot == "kb") overrides[name][0] = value;
                else if (slot == "pad") overrides[name][1] = value;
            }
        }

        for (int id = 0; id < kMaxActions; ++id)
            if (actions[id].registered && actions[id].rebindable) ApplyOverride(id);
    }

    void ApplyOverride(int id) {
        Action& a = actions[id];
        if (!a.rebindable) return;
        auto it = overrides.find(a.name);
        if (it == overrides.end()) return;
        for (int s = 0; s < kSlotCount; ++s) {
            InputBinding parsed;
            if (!it->second[s].empty() && InputBinding::Parse(it->second[s], parsed))
                a.bindings[s] = parsed;
        }
    }
};

#endif // INPUTMAP_HPP
