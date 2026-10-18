// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "libretro.h"
#include "multiplayer.h"
#include <array>
#include <cstdint>
#include <string_view>

namespace SwanControllers {
inline constexpr unsigned DualShock = RETRO_DEVICE_SUBCLASS(RETRO_DEVICE_ANALOG, 0);
inline constexpr std::array<unsigned, 8> Devices = {RETRO_DEVICE_NONE, RETRO_DEVICE_JOYPAD, DualShock,
    RETRO_DEVICE_SUBCLASS(RETRO_DEVICE_ANALOG, 1), RETRO_DEVICE_SUBCLASS(RETRO_DEVICE_ANALOG, 2),
    RETRO_DEVICE_SUBCLASS(RETRO_DEVICE_ANALOG, 3), RETRO_DEVICE_SUBCLASS(RETRO_DEVICE_LIGHTGUN, 0),
    RETRO_DEVICE_SUBCLASS(RETRO_DEVICE_MOUSE, 0)};
inline constexpr std::array<std::string_view, 8> Names = {"None", "DigitalController", "AnalogController",
    "AnalogJoystick", "NeGcon", "NeGconRumble", "NamcoGunCon", "PlayStationMouse"};
inline unsigned Parse(std::string_view name) {
    for (unsigned i = 0; i < Names.size(); ++i) if (Names[i] == name) return Devices[i];
    return DualShock;
}
inline unsigned Sanitize(unsigned device) {
    for (auto known : Devices) if (known == device) return device;
    return DualShock;
}
inline unsigned ForPort(unsigned mode, unsigned port, const std::array<unsigned, 2>& devices) {
    const int player = SwanMultiplayer::PlayerForPort(mode, port);
    return player < 0 ? RETRO_DEVICE_NONE : player < 2 ? devices[player] : DualShock;
}

/** Translate existing PS1 button/stick bindings without rewriting the user's maps. */
template<class Buttons, class Axes>
int16_t Read(unsigned device, unsigned index, unsigned id, const Buttons& buttons, const Axes& axes) {
    if (device == RETRO_DEVICE_JOYPAD) {
        if (id == RETRO_DEVICE_ID_JOYPAD_MASK) {
            unsigned mask = 0;
            for (unsigned b = 0; b < 16; ++b) if (buttons[b]) mask |= 1u << b;
            return static_cast<int16_t>(mask);
        }
        return id < 16 ? int16_t(buttons[id]) : 0;
    }
    if (device == RETRO_DEVICE_ANALOG) {
        if (index == RETRO_DEVICE_INDEX_ANALOG_BUTTON)
            return id < 16 && buttons[id] ? 32767 : 0; // neGcon pressure from digital bindings.
        return index < 2 && id < 2 ? int16_t(axes[index * 2 + id]) : 0;
    }
    if (device == RETRO_DEVICE_LIGHTGUN) {
        switch (id) {
            case RETRO_DEVICE_ID_LIGHTGUN_SCREEN_X: return axes[0];
            case RETRO_DEVICE_ID_LIGHTGUN_SCREEN_Y: return axes[1];
            case RETRO_DEVICE_ID_LIGHTGUN_TRIGGER: return buttons[RETRO_DEVICE_ID_JOYPAD_B] || buttons[RETRO_DEVICE_ID_JOYPAD_R2];
            case RETRO_DEVICE_ID_LIGHTGUN_RELOAD:
            case RETRO_DEVICE_ID_LIGHTGUN_IS_OFFSCREEN: return buttons[RETRO_DEVICE_ID_JOYPAD_A];
            case RETRO_DEVICE_ID_LIGHTGUN_AUX_A: return buttons[RETRO_DEVICE_ID_JOYPAD_X];
            case RETRO_DEVICE_ID_LIGHTGUN_AUX_B: return buttons[RETRO_DEVICE_ID_JOYPAD_Y];
            default: return 0;
        }
    }
    if (device == RETRO_DEVICE_MOUSE) {
        switch (id) {
            case RETRO_DEVICE_ID_MOUSE_X: return int16_t(axes[0]) / 2048;
            case RETRO_DEVICE_ID_MOUSE_Y: return int16_t(axes[1]) / 2048;
            case RETRO_DEVICE_ID_MOUSE_LEFT: return buttons[RETRO_DEVICE_ID_JOYPAD_B];
            case RETRO_DEVICE_ID_MOUSE_RIGHT: return buttons[RETRO_DEVICE_ID_JOYPAD_A];
            default: return 0;
        }
    }
    return 0;
}
}
