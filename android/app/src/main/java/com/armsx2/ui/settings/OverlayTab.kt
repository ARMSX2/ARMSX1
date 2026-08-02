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
 * This used to be the PS2 "On-Screen" tab: an OSD scale/colour pair plus fifteen toggles for the
 * emulator-drawn stats overlay (FPS/VPS/GS stats/GPU pipeline stats…). None of that survives the
 * PS1 port — the ARMSX1 core draws no OSD of its own (its only overlay is `[video].debug_panel`,
 * which lives on the Video tab), so those rows would have been switches wired to nothing.
 *
 * What is left is genuinely app-side and still works: [UiScale] resizes the Compose front-end, and
 * the hotkey-toast switch is a plain preference.
 */
@Composable
fun OverlayTab() {
    val scroll = settingsScrollState()
    ControllerAutoScroll(scroll)

    Column(modifier = Modifier.fillMaxWidth()) {
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
