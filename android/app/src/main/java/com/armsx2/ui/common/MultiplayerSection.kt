package com.armsx2.ui.common

import androidx.compose.runtime.Composable
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.ui.unit.dp
import com.armsx2.config.MultiplayerMode
import com.armsx2.i18n.str
import com.armsx2.ui.settings.CollapsibleSection
import com.armsx2.ui.settings.ControllerTypePicker
import com.armsx2.ui.settings.ToggleRow

/** Shared settings/quick-menu UI. Scope and persistence belong to the caller. */
@Composable
fun MultiplayerSection(mode: MultiplayerMode, onChange: (MultiplayerMode) -> Unit) {
    Column(verticalArrangement = Arrangement.spacedBy(6.dp)) {
        val custom = mode == MultiplayerMode.PORT2_ONLY
        val options = listOf(str("pad.multiplayer.standard"), str("pad.multiplayer.four"), str("pad.multiplayer.eight")) +
            if (custom) listOf(str("pad.multiplayer.custom")) else emptyList()
        ControllerTypePicker(
            player = -1, // Shared setup chooser, not a player-specific controller type.
            label = str("pad.multiplayer.label"),
            options = options,
            selectedIndex = if (custom) 3 else MultiplayerMode.presets.indexOf(mode),
            description = str("pad.multiplayer.description"),
            onSelected = { if (it in MultiplayerMode.presets.indices) onChange(MultiplayerMode.presets[it]) },
        )
        CollapsibleSection(str("pad.multiplayer.advanced"), initiallyExpanded = false) {
            ToggleRow(str("pad.multiplayer.port1"), mode.port1, description = str("pad.multiplayer.portDescription")) {
                onChange(MultiplayerMode.fromPorts(it, mode.port2))
            }
            ToggleRow(str("pad.multiplayer.port2"), mode.port2, description = str("pad.multiplayer.portDescription")) {
                onChange(MultiplayerMode.fromPorts(mode.port1, it))
            }
        }
    }
}
