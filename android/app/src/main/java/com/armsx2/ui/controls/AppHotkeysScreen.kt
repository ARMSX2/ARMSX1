package com.armsx2.ui.controls

import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.CompositionLocalProvider
import androidx.compose.runtime.DisposableEffect
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.dp
import com.armsx2.i18n.str
import com.armsx2.input.AppHotkeyAction
import com.armsx2.input.AppHotkeys
import com.armsx2.input.ControllerMappings
import com.armsx2.ui.common.ArmsBackdrop
import com.armsx2.ui.common.ArmsTopBar
import com.armsx2.ui.common.RoundAction
import com.armsx2.ui.settings.*

@Composable
fun AppHotkeysScreen(onBack: () -> Unit) {
    val scroll = rememberScrollState()
    ControllerAutoScroll(scroll)
    @Suppress("UNUSED_EXPRESSION") AppHotkeys.revision.intValue
    DisposableEffect(Unit) { onDispose { AppHotkeys.cancelCapture() } }
    ArmsBackdrop {
        CompositionLocalProvider(LocalSettingsScrollState provides scroll) {
            Column(Modifier.fillMaxSize().verticalScroll(scroll)) {
                ArmsTopBar(title = str("app.hotkeys.title"), leading = { RoundAction("←", str("action.back"), onBack) })
                Column(Modifier.padding(12.dp)) {
                    Text(str("app.hotkeys.modifier"), style = MaterialTheme.typography.titleMedium,
                        color = MaterialTheme.colorScheme.primary)
                    Text(str("app.hotkeys.description"), style = MaterialTheme.typography.bodyMedium,
                        color = MaterialTheme.colorScheme.onSurfaceVariant, modifier = Modifier.padding(vertical = 8.dp))
                    AppHotkeyAction.entries.forEach { action ->
                        val capturing = AppHotkeys.capture.value == action
                        val binding = AppHotkeys.code(action)
                        val start = { AppHotkeys.beginCapture(action) }
                        Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
                            Column(Modifier.weight(1f).controllerFocusable("app-hotkey:${action.id}", onConfirm = start)
                                .clickable(onClick = start).padding(vertical = 16.dp, horizontal = 8.dp)) {
                                Text(str(action.labelKey), style = MaterialTheme.typography.titleMedium)
                                Text(if (capturing) str(if (AppHotkeys.modifierHeld.value) "app.hotkeys.pressControl" else "app.hotkeys.holdSelect")
                                    else if (binding == 0) str("app.hotkeys.unassigned")
                                    else "${ControllerMappings.labelForKey(AppHotkeys.modifier(action))} + ${ControllerMappings.labelForKey(binding)}",
                                    color = MaterialTheme.colorScheme.primary, style = MaterialTheme.typography.bodyMedium)
                            }
                            val clear = { if (capturing) AppHotkeys.cancelCapture() else AppHotkeys.clear(action) }
                            TextButton(onClick = clear, modifier = Modifier.controllerFocusable("app-hotkey-clear:${action.id}", onConfirm = clear)) {
                                Text(str(if (capturing) "action.cancel" else "setup.button.clear"))
                            }
                        }
                        SettingsDivider()
                    }
                }
            }
        }
    }
}
