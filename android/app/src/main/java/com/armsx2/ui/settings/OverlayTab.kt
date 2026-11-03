package com.armsx2.ui.settings

import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.core.content.edit
import com.armsx2.i18n.str
import com.armsx2.ui.UiScale

/**
 * Interface tab — how big the launcher's own chrome is drawn.
 *
 * Performance text is drawn by GameOsdOverlay, not the retired native debug_panel.
 * Its mode/Custom choices share one store with Video and the quick menu.
 */
@Composable
fun OverlayTab(gameKey: String? = null, contextGameKey: String? = gameKey) {
    val scroll = settingsScrollState()
    ControllerAutoScroll(scroll)

    Column(modifier = Modifier.fillMaxWidth()) {
        PerformanceOverlaySettings(gameKey, contextGameKey)
        SettingsDivider()
        Text(
            str("overlay.interfaceScaling.description"),
            color = MaterialTheme.colorScheme.onSurfaceVariant,
            fontSize = 14.sp,
            modifier = Modifier.padding(bottom = 8.dp),
        )
        IntSliderRow(
            label = str("overlay.uiSize.label"),
            value = (UiScale.borderScale.value * 100f).toInt(),
            min = (UiScale.MIN * 100f).toInt(),
            max = (UiScale.BORDER_MAX * 100f).toInt(),
            description = str("overlay.uiSize.description"),
            valueFormatter = { "$it%" },
            onChange = { UiScale.setBorderScale(it / 100f) },
        )
        SettingsDivider()
        IntSliderRow(
            label = str("overlay.uiFontSize.label"),
            value = (UiScale.fontScale.value * 100f).toInt(),
            min = (UiScale.MIN * 100f).toInt(),
            max = (UiScale.MAX * 100f).toInt(),
            description = str("overlay.uiFontSize.description"),
            valueFormatter = { "$it%" },
            onChange = { UiScale.setFontScale(it / 100f) },
        )
        SettingsDivider()
        // Android hotkey pop-ups (Fast-Forward on/off, etc.) — a plain preference, unrelated to
        // any emulator OSD, so it is still live.
        val ffToasts = remember {
            mutableStateOf(com.armsx2.runtime.MainActivityRuntime.prefs.getBoolean("ui.hotkeyToasts", true))
        }
        ToggleRow(str("overlay.toggle.fastForwardPopups"), ffToasts.value) {
            ffToasts.value = it
            com.armsx2.runtime.MainActivityRuntime.prefs.edit { putBoolean("ui.hotkeyToasts", it) }
        }
    }
}
