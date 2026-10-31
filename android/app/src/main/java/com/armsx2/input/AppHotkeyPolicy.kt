package com.armsx2.input

enum class AppHotkeyAction(val id: String, val labelKey: String) {
    CONTROLS("controls", "tab.controls"),
    BIOS("bios", "setup.step.bios.title"),
    SAVES("saves", "savestate.title.loadManage"),
    LIBRARY("library", "app.hotkeys.library"),
    SETTINGS("settings", "action.settings"),
}

data class AppHotkeyBinding(val modifier: Int, val code: Int)

/** Ordered, same-device hold chords. This policy never intercepts ordinary gameplay. */
class AppHotkeyPolicy(private val select: Int, private val start: Int, val holdMs: Long = 350L) {
    data class Result(val consumed: Boolean, val binding: AppHotkeyBinding? = null,
        val action: AppHotkeyAction? = null, val tapStart: Boolean = false)
    private data class Hold(val modifier: Int, val started: Long, val scanCode: Int, var used: Boolean = false)
    private val holds = mutableMapOf<Int, MutableMap<Int, Hold>>()
    private val consumed = mutableSetOf<Pair<Int, Int>>()

    fun holdsModifier(deviceId: Int): Boolean = holds[deviceId]?.isNotEmpty() == true
    val anyModifierHeld: Boolean get() = holds.values.any { it.isNotEmpty() }
    fun clearModifiers() { holds.clear() }
    fun reset() { clearModifiers(); consumed.clear() }

    fun process(code: Int, deviceId: Int, scanCode: Int, down: Boolean, newPress: Boolean,
        eligible: Boolean, capturing: Boolean, now: Long, bindings: Map<AppHotkeyBinding, AppHotkeyAction>): Result {
        val identity = deviceId to code
        if (!down) {
            val hold = holds[deviceId]?.remove(code)
            val tap = eligible && !capturing && code == start && hold != null && !hold.used && now - hold.started < holdMs
            if (consumed.remove(identity)) return Result(true, tapStart = tap)
        }
        if (!eligible) { clearModifiers(); return Result(false) }
        if (code == 0) return Result(capturing)
        if (down && !newPress && identity in consumed) return Result(true)
        if (code == select || code == start) {
            if (down && newPress) {
                val deviceHolds = holds.getOrPut(deviceId) { linkedMapOf() }
                deviceHolds.values.forEach { it.used = true }
                deviceHolds[code] = Hold(code, now, scanCode, used = deviceHolds.isNotEmpty())
                consumed.add(identity)
            }
            return Result(true)
        }
        val deviceHolds = holds[deviceId].orEmpty()
        val hold = deviceHolds.values.singleOrNull()
        if (capturing || deviceHolds.isNotEmpty()) {
            if (down) consumed.add(identity)
            if (down) deviceHolds.values.forEach { it.used = true }
            // Unmatched/early chords also own their buttons, preventing accidental A/B/L/R actions.
            val ordered = down && newPress && hold != null &&
                (scanCode == 0 || scanCode != hold.scanCode)
            if (!ordered) return Result(true)
            val chord = AppHotkeyBinding(hold.modifier, code)
            // A saved chord is already deliberate: its modifier was pressed
            // first and is still held. Fire on the primary DOWN without a timer.
            // Keep the hold threshold only for binding capture and Start alone.
            if (capturing && now - hold.started < holdMs) return Result(true)
            return if (capturing) Result(true, binding = chord)
                else Result(true, action = bindings[chord])
        }
        return Result(false)
    }
}
