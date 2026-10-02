#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

enum InputMask : uint8_t {
    INPUT_NONE = 0,
    INPUT_LEFT = 1 << 0,
    INPUT_RIGHT = 1 << 1,
    INPUT_TOP = 1 << 2,
    INPUT_DOWN = 1 << 3,
    INPUT_SHOOT = 1 << 4
};

// InputBlob layout (4 bytes):
//   data[0]             InputMask bits: which actions are pressed.
//   data[1] / [2] / [3] how far: turn left / turn right / thrust, 1..255 (255 = full). Keys always send 255; a
//                       gamepad stick or trigger sends less when partly pushed.
// The bits stay the source of truth for "pressed"; the intensity only scales the effect. An intensity of 0 with the
// bit set (a zero-filled blob, e.g. the server's neutral input) counts as full.
enum InputAnalogByte : int {
    INPUT_BYTE_LEFT = 1,
    INPUT_BYTE_RIGHT = 2,
    INPUT_BYTE_THRUST = 3,
};

// Steps an intensity is quantized to before it goes on the wire: a stick held still doesn't jitter between values
// (every change is a new input to send and, for the other players, a misprediction), and 16 levels are plenty.
constexpr int INPUT_ANALOG_STEPS = 16;

// Client: 0..1 intensity -> wire byte. Never 0 for a pressed action (0 would read as "full", see above).
inline uint8_t EncodeInputIntensity(float value) {
    const int step = std::clamp(static_cast<int>(std::lround(value * INPUT_ANALOG_STEPS)), 1, INPUT_ANALOG_STEPS);
    return static_cast<uint8_t>(step * 255 / INPUT_ANALOG_STEPS);
}

// Simulation (client prediction and server alike): wire byte -> 0..1 scale, only meaningful with the bit set.
// 255 gives exactly 1.0f, so keyboard input simulates exactly as before analog input existed.
inline float DecodeInputIntensity(uint8_t byte) {
    return byte == 0 ? 1.0f : static_cast<float>(byte) / 255.0f;
}
