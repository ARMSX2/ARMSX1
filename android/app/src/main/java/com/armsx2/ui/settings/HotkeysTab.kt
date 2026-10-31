package com.armsx2.ui.settings

import androidx.compose.foundation.ScrollState
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.runtime.Composable
import androidx.compose.runtime.MutableState
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.armsx2.config.Settings
import com.armsx2.i18n.str
import com.armsx2.input.AppHotkeyAction
import com.armsx2.input.AppHotkeys
import com.armsx2.input.ControllerMappings
import com.armsx2.runtime.MainActivityRuntime
import androidx.core.content.edit

/**
 * Dedicated controller-hotkey binding tab. Pulled out of the Pad tab so the hotkeys have a
 * home that's easy to find. Binding happens via [ControllerMappings.captureHotkey] —
 * tapping a row arms it, and the next button seen by MainActivityRuntime.dispatchKeyEvent
 * is bound to it.
 *
 * Lists [ControllerMappings.ps1Hotkeys], the actions implemented by the PS1 runtime.
 * The binding rows are shared with the quick menu's Controls pane.
 */
@Composable
fun HotkeysTab(@Suppress("UNUSED_PARAMETER") state: MutableState<Settings>) {
    val scroll = settingsScrollState()
    ControllerAutoScroll(scroll)

    Column(
        modifier = Modifier
            .fillMaxWidth(),
    ) {
        HotkeyBindings()
        AppHotkeyBindings()
        // #384: the Android system Back button / gesture opens the in-game menu (like Nether/Eden).
        // For devices where Back is hijacked (e.g. Assistant), bind any button to the "Menu / Pause"
        // hotkey above instead. Controller Circle is unaffected.
        val backOpensMenu = remember {
            mutableStateOf(MainActivityRuntime.prefs.getBoolean("input.backOpensMenu", true))
        }
        ToggleRow(
            str("hotkeys.backOpensMenu.label"),
            backOpensMenu.value,
            description = str("hotkeys.backOpensMenu.description"),
        ) { v ->
            backOpensMenu.value = v
            MainActivityRuntime.prefs.edit { putBoolean("input.backOpensMenu", v) }
        }
        SettingsDivider()
        // Closing a game opened from a frontend (ES-DE etc.) returns to that
        // frontend instead of the ARMSX2 library.
        val exitToLauncher = remember {
            mutableStateOf(MainActivityRuntime.prefs.getBoolean("ui.exitToLauncherExternal", true))
        }
        ToggleRow(
            str("hotkeys.exitToLauncher.label"),
            exitToLauncher.value,
            description = str("hotkeys.exitToLauncher.description"),
        ) { v ->
            exitToLauncher.value = v
            MainActivityRuntime.prefs.edit { putBoolean("ui.exitToLauncherExternal", v) }
        }
        SettingsDivider()
        @Suppress("UNUSED_EXPRESSION") Box(Modifier.height(6.dp))
    }
}

/**
 * App hotkeys: hold Select or Start and press a button to jump to a screen from the menus. They had
 * a page of their own in the library's sidebar, which left them reachable only from the library;
 * they sit with the other hotkeys in Settings now, as in ARMSX2/ARMSX3.
 */
@Composable
private fun AppHotkeyBindings() {
    @Suppress("UNUSED_EXPRESSION") AppHotkeys.revision.intValue
    androidx.compose.runtime.DisposableEffect(Unit) { onDispose { AppHotkeys.cancelCapture() } }
    Column(Modifier.fillMaxWidth()) {
        Text(
            str("app.hotkeys.title"),
            color = MaterialTheme.colorScheme.onSurface,
            fontSize = 16.sp,
            fontWeight = FontWeight.Bold,
            modifier = Modifier.padding(start = 6.dp, end = 6.dp, top = 14.dp, bottom = 2.dp),
        )
        Text(
            str("app.hotkeys.description"),
            color = MaterialTheme.colorScheme.onSurfaceVariant,
            fontSize = 14.sp,
            modifier = Modifier.padding(horizontal = 6.dp, vertical = 4.dp),
        )
        AppHotkeyAction.entries.forEach { action ->
            val capturing = AppHotkeys.capture.value == action
            val binding = AppHotkeys.code(action)
            val start = { AppHotkeys.beginCapture(action) }
            val clearOrCancel: () -> Unit = { if (capturing) AppHotkeys.cancelCapture() else AppHotkeys.clear(action) }
            Row(
                modifier = Modifier
                    .fillMaxWidth()
                    .heightIn(min = 72.dp)
                    .clip(RoundedCornerShape(16.dp))
                    .background(rowAura())
                    .clickable(onClick = start)
                    .controllerFocusable(controllerId = "app-hotkey:${action.id}", onConfirm = start)
                    .padding(horizontal = 6.dp),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                Column(Modifier.weight(1f).padding(vertical = 10.dp)) {
                    Text(str(action.labelKey), color = MaterialTheme.colorScheme.onSurface, fontSize = 16.sp,
                        fontWeight = FontWeight.SemiBold)
                    Text(
                        when {
                            capturing -> str(if (AppHotkeys.modifierHeld.value) "app.hotkeys.pressControl" else "app.hotkeys.holdSelect")
                            binding == 0 -> str("app.hotkeys.unassigned")
                            else -> "${ControllerMappings.labelForKey(AppHotkeys.modifier(action))} + ${ControllerMappings.labelForKey(binding)}"
                        },
                        color = if (capturing) Color(0xFFFFD33A) else Color(0xFFCCCCCC),
                        fontSize = 14.sp, fontWeight = FontWeight.Bold,
                        maxLines = 2, overflow = TextOverflow.Ellipsis,
                    )
                }
                if (binding != 0 || capturing) {
                    Text(
                        str(if (capturing) "action.cancel" else "hotkeys.clear"),
                        color = Color(0xFFFF6B6B),
                        fontSize = 14.sp,
                        fontWeight = FontWeight.Bold,
                        modifier = Modifier
                            .clickable(onClick = clearOrCancel)
                            .controllerFocusable("app-hotkey-clear:${action.id}", onConfirm = clearOrCancel)
                            .padding(end = 10.dp),
                    )
                }
            }
            SettingsDivider()
        }
    }
}

/** Shared with the in-game Controls pane; both screens edit the same persisted bindings. */
@Composable
fun HotkeyBindings(showHeader: Boolean = true) {
    androidx.compose.runtime.DisposableEffect(Unit) {
        onDispose { ControllerMappings.endHotkeyCapture() }
    }
    Column(Modifier.fillMaxWidth()) {
        if (showHeader) Text(
            str("hotkeys.header"),
            color = MaterialTheme.colorScheme.onSurface,
            fontSize = 16.sp,
            fontWeight = FontWeight.Bold,
            modifier = Modifier.padding(horizontal = 6.dp, vertical = 8.dp),
        )
        // PS1 port: only the hotkeys the ARMSX core can actually serve are listed. The
        // ControllerMappings.ps1Hotkeys documents the supported actions; both runtime
        // dispatch paths handle each action listed here.
        ControllerMappings.ps1Hotkeys.forEach { hk ->
            @Suppress("UNUSED_EXPRESSION") ControllerMappings.hotkeyBindTick.value
            val capturing = ControllerMappings.captureHotkey.value == hk
            val binding = ControllerMappings.hotkeyLabel(hk)
            val unset = binding.isEmpty()
            val clearOrCancel: () -> Unit = {
                if (capturing) ControllerMappings.endHotkeyCapture() else {
                    ControllerMappings.clearHotkey(hk)
                    ControllerMappings.hotkeyBindTick.value++
                }
            }
            Row(
                modifier = Modifier
                    .fillMaxWidth()
                    .heightIn(min = 72.dp)
                    .clip(RoundedCornerShape(16.dp))
                    .background(rowAura())
                    .clickable { ControllerMappings.beginHotkeyCapture(hk) }
                    .controllerFocusable(
                        controllerId = "hotkey:${hk.name}",
                        onConfirm = { ControllerMappings.beginHotkeyCapture(hk) },
                    )
                    .padding(horizontal = 6.dp),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                Column(Modifier.weight(1f).padding(vertical = 10.dp)) {
                    Text(hk.label, color = MaterialTheme.colorScheme.onSurface, fontSize = 16.sp,
                        fontWeight = FontWeight.SemiBold)
                    Text(
                        when {
                            capturing -> if (ControllerMappings.captureButtons.value.isEmpty()) str("hotkeys.capturePrompt")
                                else ControllerMappings.captureButtons.value.joinToString(" + ", transform = ControllerMappings::labelForKey) +
                                    " — " + str("hotkeys.releasePrompt")
                            unset -> str("hotkeys.notSet")
                            else -> binding
                        },
                        color = if (capturing) Color(0xFFFFD33A) else Color(0xFFCCCCCC),
                        fontSize = 14.sp, fontWeight = FontWeight.Bold,
                        maxLines = 2, overflow = TextOverflow.Ellipsis,
                    )
                }
                if (!unset || capturing) {
                    Text(
                        str(if (capturing) "action.cancel" else "hotkeys.clear"),
                        color = Color(0xFFFF6B6B),
                        fontSize = 14.sp,
                        fontWeight = FontWeight.Bold,
                        modifier = Modifier
                            .clickable(onClick = clearOrCancel)
                            .controllerFocusable("hotkey.clear:${hk.name}", onConfirm = clearOrCancel)
                            .padding(end = 10.dp),
                    )
                }
            }
            SettingsDivider()
        }
    }
}
