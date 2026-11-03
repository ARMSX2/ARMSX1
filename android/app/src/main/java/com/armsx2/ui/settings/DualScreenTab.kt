package com.armsx2.ui.settings

import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.input.PasswordVisualTransformation
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.armsx2.core.Ps1GameMedia
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

        // Media behind the box art while browsing. Each source can be dropped on its own.
        SettingsDivider()
        ToggleRow(
            label = str("dualScreen.media.snaps.label"),
            value = Ps1GameMedia.showSnaps.value,
            description = str("dualScreen.media.snaps.description"),
        ) { Ps1GameMedia.setShowSnaps(it) }
        SettingsDivider()
        ToggleRow(
            label = str("dualScreen.media.titles.label"),
            value = Ps1GameMedia.showTitles.value,
            description = str("dualScreen.media.titles.description"),
        ) { Ps1GameMedia.setShowTitles(it) }
        SettingsDivider()
        ToggleRow(
            label = str("dualScreen.media.fanArt.label"),
            value = Ps1GameMedia.showFanArt.value,
            description = str("dualScreen.media.fanArt.description"),
        ) { Ps1GameMedia.setShowFanArt(it) }
        if (Ps1GameMedia.showFanArt.value) {
            // Inline field, never a dialog: a Dialog is its own focused window and swallows pad keys.
            val draft = remember { mutableStateOf(Ps1GameMedia.steamGridDbKey.value) }
            OutlinedTextField(
                value = draft.value,
                onValueChange = {
                    draft.value = it
                    Ps1GameMedia.setSteamGridDbKey(it)
                },
                singleLine = true,
                label = { Text(str("dualScreen.media.fanArt.key")) },
                visualTransformation = PasswordVisualTransformation(),
                modifier = Modifier.fillMaxWidth().padding(vertical = 8.dp),
            )
            Ps1GameMedia.steamGridDbError.value?.let {
                Text(it, color = MaterialTheme.colorScheme.error, fontSize = 13.sp)
            }
        }
    }
}
