#pragma once

#include "Utils/InputMap.hpp"

// The game's actions (InputMap ids from kFirstGameAction up). The ship ones feed GenerateLocalInput, which turns them
// into the networked InputMask bits: the wire format and the simulation don't know which device pressed what.
namespace GameAction {
    enum : int {
        TurnLeft = InputMap::kFirstGameAction,
        TurnRight,
        Thrust,
        Shoot,
        Pause,
        SpectatePrev,
        SpectateNext,
        Dash,
        Shield,
    };
}

// Client only, after Debug::Initialize (it sets the user data folder input_bindings.cfg is read from).
inline void RegisterGameActions()
{
    using B = InputBinding;
    InputMap& map = InputMap::Get();

    // gameplay = true: they go quiet while the UI owns the input (pause menu open, text field focused).
    map.RegisterAction(GameAction::TurnLeft, "turn_left", "Girar a la izquierda",
        B::Key(GLFW_KEY_A), B::PadAxis(GLFW_GAMEPAD_AXIS_LEFT_X, -1), true);
    map.RegisterAction(GameAction::TurnRight, "turn_right", "Girar a la derecha",
        B::Key(GLFW_KEY_D), B::PadAxis(GLFW_GAMEPAD_AXIS_LEFT_X, 1), true);
    map.RegisterAction(GameAction::Thrust, "thrust", "Acelerar",
        B::Key(GLFW_KEY_W), B::PadAxis(GLFW_GAMEPAD_AXIS_RIGHT_TRIGGER, 1), true);
    map.RegisterAction(GameAction::Shoot, "shoot", "Disparar",
        B::Key(GLFW_KEY_SPACE), B::PadButton(GLFW_GAMEPAD_BUTTON_A), true);
    map.RegisterAction(GameAction::Dash, "dash", "Propulsión",
        B::Key(GLFW_KEY_LEFT_SHIFT), B::PadButton(GLFW_GAMEPAD_BUTTON_X), true);
    map.RegisterAction(GameAction::Shield, "shield", "Escudo láser",
        B::Key(GLFW_KEY_E), B::PadButton(GLFW_GAMEPAD_BUTTON_B), true);

    // Not gameplay: Pause has to work while the pause menu blocks gameplay, and spectating happens once dead.
    map.RegisterAction(GameAction::Pause, "pause", "Menú de pausa",
        B::Key(GLFW_KEY_ESCAPE), B::PadButton(GLFW_GAMEPAD_BUTTON_START), false);
    map.RegisterAction(GameAction::SpectatePrev, "spectate_prev", "Espectador: jugador anterior",
        B::Key(GLFW_KEY_LEFT), B::PadButton(GLFW_GAMEPAD_BUTTON_LEFT_BUMPER), false);
    map.RegisterAction(GameAction::SpectateNext, "spectate_next", "Espectador: jugador siguiente",
        B::Key(GLFW_KEY_RIGHT), B::PadButton(GLFW_GAMEPAD_BUTTON_RIGHT_BUMPER), false);

    // The D-pad steers too, next to whatever the stick binding is.
    map.AddFixedBinding(GameAction::TurnLeft, B::PadButton(GLFW_GAMEPAD_BUTTON_DPAD_LEFT));
    map.AddFixedBinding(GameAction::TurnRight, B::PadButton(GLFW_GAMEPAD_BUTTON_DPAD_RIGHT));
}
