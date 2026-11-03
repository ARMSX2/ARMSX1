package com.armsx2.ui.settings

import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import com.armsx2.i18n.str
import com.armsx2.runtime.DualScreen

/**
 * Dual Screen tab — second-display play for handhelds such as the AYN Thor.
 *
 * Global, not per game: it describes the device, and Off keeps single-screen play exactly as it was.
 */
@Composable
fun DualScreenTab() {
    val scroll = settingsScrollState()
    ControllerAutoScroll(scroll)

    Column(modifier = Modifier.fillMaxWidth()) {
        val modes = DualScreen.Mode.entries
        SegmentedRow(
            label = str("overlay.dualScreen.label"),
            options = listOf(
                str("overlay.dualScreen.off"),
                str("overlay.dualScreen.gameMain"),
                str("overlay.dualScreen.gameSecond"),
            ),
            selectedIndex = modes.indexOf(DualScreen.mode.value),
            description = str("overlay.dualScreen.description"),
            onChange = { idx -> modes.getOrNull(idx)?.let { DualScreen.setMode(it) } },
        )
        SettingsDivider()
        ToggleRow(
            label = str("dualScreen.infoPanel.label"),
            value = DualScreen.infoPanel.value,
            description = str("dualScreen.infoPanel.description"),
        ) { DualScreen.setInfoPanel(it) }
        SettingsDivider()
        ToggleRow(
            label = str("dualScreen.libraryArt.label"),
            value = DualScreen.libraryArt.value,
            description = str("dualScreen.libraryArt.description"),
        ) { DualScreen.setLibraryArt(it) }
    }
}
