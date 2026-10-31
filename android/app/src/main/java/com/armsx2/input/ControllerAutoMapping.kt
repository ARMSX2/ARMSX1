package com.armsx2.input

import android.view.KeyEvent

/** Android-normalized gamepad keys, not guesses based on a controller's marketing name. */
internal object ControllerAutoMapping {
    val standardButtons = linkedMapOf(
        "dpad_up" to KeyEvent.KEYCODE_DPAD_UP, "dpad_down" to KeyEvent.KEYCODE_DPAD_DOWN,
        "dpad_left" to KeyEvent.KEYCODE_DPAD_LEFT, "dpad_right" to KeyEvent.KEYCODE_DPAD_RIGHT,
        "cross" to KeyEvent.KEYCODE_BUTTON_A, "circle" to KeyEvent.KEYCODE_BUTTON_B,
        "square" to KeyEvent.KEYCODE_BUTTON_X, "triangle" to KeyEvent.KEYCODE_BUTTON_Y,
        "l1" to KeyEvent.KEYCODE_BUTTON_L1, "r1" to KeyEvent.KEYCODE_BUTTON_R1,
        "l2" to KeyEvent.KEYCODE_BUTTON_L2, "r2" to KeyEvent.KEYCODE_BUTTON_R2,
        "l3" to KeyEvent.KEYCODE_BUTTON_THUMBL, "r3" to KeyEvent.KEYCODE_BUTTON_THUMBR,
        "select" to KeyEvent.KEYCODE_BUTTON_SELECT, "start" to KeyEvent.KEYCODE_BUTTON_START,
    )

    data class Capabilities(
        val keys: Set<Int>,
        val hatX: Boolean = false, val hatY: Boolean = false,
        val leftTrigger: Boolean = false, val rightTrigger: Boolean = false,
        val leftStick: Boolean = false, val rightStick: Boolean = false,
    )

    data class Plan(val buttons: Map<String, Int>, val leftStick: Boolean, val rightStick: Boolean) {
        val mappedButtons: Int get() = buttons.values.count { it != KeyEvent.KEYCODE_UNKNOWN }
        val usable: Boolean get() = mappedButtons > 0 || leftStick || rightStick

        /** Explicitly pin the chosen tier so old overrides cannot resurrect duplicate binds. */
        fun preferenceValues(player: Int, serial: String?, actions: List<String>): Map<String, Any> {
            require(player in 0..1)
            val prefix = if (player == 1) "p2." else ""
            fun key(base: String) = if (serial.isNullOrEmpty()) prefix + base else "game.$serial.$prefix$base"
            return buildMap {
                actions.forEach { put(key("pad.map.$it"), buttons[it] ?: KeyEvent.KEYCODE_UNKNOWN) }
                if (leftStick) put(key("pad.lstick.mode"), "analog")
                if (rightStick) put(key("pad.rstick.mode"), "analog")
            }
        }
    }

    fun plan(capabilities: Capabilities): Plan {
        val bindings = standardButtons.mapValues { (action, key) ->
            val fromAxis = when (action) {
                "dpad_left", "dpad_right" -> capabilities.hatX
                "dpad_up", "dpad_down" -> capabilities.hatY
                "l2" -> capabilities.leftTrigger
                "r2" -> capabilities.rightTrigger
                else -> false
            }
            if (key in capabilities.keys || fromAxis) key else KeyEvent.KEYCODE_UNKNOWN
        }
        return Plan(bindings, capabilities.leftStick, capabilities.rightStick)
    }
}
