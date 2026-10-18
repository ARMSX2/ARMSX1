package com.armsx2.input

import android.os.SystemClock
import android.view.KeyEvent
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.core.content.edit
import com.armsx2.runtime.MainActivityRuntime

/** App navigation has its own keys; game bindings and existing emulator hotkeys are never rewritten. */
object AppHotkeys {
    const val SELECT = KeyEvent.KEYCODE_BUTTON_SELECT
    const val START = KeyEvent.KEYCODE_BUTTON_START
    private val policy = AppHotkeyPolicy(SELECT, START)
    val capture = mutableStateOf<AppHotkeyAction?>(null)
    val modifierHeld = mutableStateOf(false)
    val revision = mutableIntStateOf(0)
    fun preferenceKey(action: AppHotkeyAction) = "app.hotkeys.${action.id}"
    fun code(action: AppHotkeyAction): Int = MainActivityRuntime.prefs.getInt(preferenceKey(action), 0)
    fun modifier(action: AppHotkeyAction): Int = MainActivityRuntime.prefs.getInt(preferenceKey(action) + ".modifier", SELECT)
        .takeIf { it == SELECT || it == START } ?: SELECT
    fun bind(action: AppHotkeyAction, code: Int, modifier: Int) {
        require(code != 0 && code != SELECT && code != START && modifier in setOf(SELECT, START))
        MainActivityRuntime.prefs.edit {
            AppHotkeyAction.entries.filter { it != action && AppHotkeys.code(it) == code && AppHotkeys.modifier(it) == modifier }
                .forEach { putInt(preferenceKey(it), 0) }
            putInt(preferenceKey(action), code)
            putInt(preferenceKey(action) + ".modifier", modifier)
        }
        revision.intValue++
    }
    fun clear(action: AppHotkeyAction) {
        MainActivityRuntime.prefs.edit { putInt(preferenceKey(action), 0) }
        cancelCapture()
        revision.intValue++
    }
    fun beginCapture(action: AppHotkeyAction) {
        policy.clearModifiers() // Require a fresh Select press after choosing the row.
        modifierHeld.value = false
        capture.value = action
    }
    fun cancelCapture() {
        // AnimatedContent disposes the outgoing page after navigation. If no
        // binding is being edited, that cleanup must not cancel a fresh hold
        // already started on the destination page.
        if (capture.value == null) return
        capture.value = null
        policy.clearModifiers()
        modifierHeld.value = false
    }
    fun resetInput() {
        capture.value = null
        policy.reset()
        modifierHeld.value = false
    }
    fun holdsModifier(deviceId: Int): Boolean = policy.holdsModifier(deviceId)

    private fun bindings(): Map<AppHotkeyBinding, AppHotkeyAction> =
        AppHotkeyAction.entries.mapNotNull { action -> code(action).takeIf { it != 0 && it != SELECT && it != START }
            ?.let { AppHotkeyBinding(modifier(action), it) to action } }.toMap()

    fun handle(event: KeyEvent, newPress: Boolean, eligible: Boolean, onAction: (AppHotkeyAction) -> Unit,
        onStartTap: () -> Unit): Boolean {
        val capturing = capture.value
        if (eligible && capturing != null && newPress &&
            event.keyCode in setOf(KeyEvent.KEYCODE_BUTTON_B, KeyEvent.KEYCODE_BACK) &&
            !policy.holdsModifier(event.deviceId)) {
            cancelCapture()
            // Still drain this cancel button's UP through the ordinary policy.
            policy.process(event.keyCode, event.deviceId, event.scanCode, true, true, true, true,
                SystemClock.uptimeMillis(), emptyMap())
            return true
        }
        val bindings = if (eligible && capturing == null) bindings() else emptyMap()
        val result = policy.process(event.keyCode, event.deviceId, event.scanCode,
            event.action == KeyEvent.ACTION_DOWN, newPress, eligible, capturing != null,
            SystemClock.uptimeMillis(), bindings)
        modifierHeld.value = policy.anyModifierHeld
        if (result.binding != null && capturing != null) {
            bind(capturing, result.binding.code, result.binding.modifier)
            cancelCapture()
        }
        result.action?.let(onAction)
        if (result.tapStart) onStartTap()
        return result.consumed
    }
}
