package com.armsx2.config

/** Devices implemented by the pinned SwanStation core. IDs are its libretro device ABI. */
enum class Ps1ControllerType(val configValue: String, val deviceId: Int, val labelKey: String) {
    NONE("None", 0, "pad.controllerType.none"),
    DIGITAL("DigitalController", 1, "pad.controllerType.digital"),
    DUALSHOCK("AnalogController", 261, "pad.controllerType.dualshock"),
    ANALOG_JOYSTICK("AnalogJoystick", 517, "pad.controllerType.joystick"),
    NEGCON("NeGcon", 773, "pad.controllerType.negcon"),
    NEGCON_RUMBLE("NeGconRumble", 1029, "pad.controllerType.negconRumble"),
    GUNCON("NamcoGunCon", 260, "pad.controllerType.guncon"),
    MOUSE("PlayStationMouse", 258, "pad.controllerType.mouse");

    companion object {
        fun fromConfig(value: String): Ps1ControllerType =
            entries.firstOrNull { it.configValue == value } ?: DUALSHOCK
    }
}
