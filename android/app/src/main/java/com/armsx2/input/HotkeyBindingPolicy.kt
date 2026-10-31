package com.armsx2.input

data class CapturedHotkey(val keyCode: Int, val modifierCode: Int = 0)

/** No timing threshold: separate physical buttons can arrive in the same input packet. */
class HotkeyCapturePolicy {
    private data class Button(val code: Int, val deviceId: Int, val scanCode: Int)
    private val held = linkedMapOf<Pair<Int, Int>, Button>()
    private val buttons = mutableListOf<Button>()
    val codes: List<Int> get() = buttons.map { it.code }

    fun reset() { held.clear(); buttons.clear() }

    fun down(code: Int, deviceId: Int, scanCode: Int, repeat: Boolean = false) {
        if (code == 0 || repeat) return
        if (buttons.isNotEmpty() && buttons[0].deviceId != deviceId) return
        val button = Button(code, deviceId, scanCode)
        if (held.put(deviceId to code, button) != null) return
        // Some drivers emit key aliases for the same physical scan code. Timing is
        // insufficient to distinguish those aliases from a real simultaneous chord.
        val alias = scanCode != 0 && buttons.any { it.deviceId == deviceId && it.scanCode == scanCode }
        if (!alias && buttons.size < 2 && buttons.none { it.code == code }) buttons.add(button)
    }

    fun up(code: Int, deviceId: Int): CapturedHotkey? {
        if (held.remove(deviceId to code) == null || held.isNotEmpty() || buttons.isEmpty()) return null
        val result = if (buttons.size == 1) CapturedHotkey(buttons[0].code)
            else CapturedHotkey(buttons[1].code, buttons[0].code)
        reset()
        return result
    }
}

object HotkeyChordPolicy {
    fun matches(key: Int, modifier: Int, pressed: Int, held: Set<Int>): Boolean =
        key != 0 && modifier != 0 && key != modifier &&
            ((pressed == key && modifier in held) || (pressed == modifier && key in held))
}
