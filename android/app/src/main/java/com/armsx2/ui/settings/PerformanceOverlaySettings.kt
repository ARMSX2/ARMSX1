package com.armsx2.ui.settings

import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import com.armsx2.config.ConfigStore
import com.armsx2.config.Settings
import com.armsx2.config.SettingsScope
import com.armsx2.i18n.str
import com.armsx2.ui.InGameOverlay

/** These controls write the Compose OSD's actual store, not the retired core debug_panel flag. */
@Composable
internal fun PerformanceOverlaySettings(gameKey: String?, contextGameKey: String? = gameKey) {
    val scope = if (gameKey == null) SettingsScope.Global else SettingsScope.Game
    val modes = InGameOverlay.OsdMode.entries
    val mode = InGameOverlay.configuredOsdMode(scope, gameKey)
    SegmentedRow(
        label = str("renderer.debugPanel.label"),
        options = modes.map { InGameOverlay.osdModeLabel(it) },
        selectedIndex = modes.indexOf(mode),
        description = str("renderer.debugPanel.description") + " " +
            str(if (gameKey == null) "overlay.osd.allGames" else "overlay.osd.thisGame"),
        onChange = { InGameOverlay.setOsdMode(modes[it], scope, gameKey) },
    )
    if (gameKey != null) {
        if (InGameOverlay.hasGameOsdOverride(gameKey)) {
            val inherit = { InGameOverlay.clearGameOsdOverride(gameKey) }
            TextButton(onClick = inherit,
                modifier = Modifier.controllerFocusable("settings.osd.inherit", onConfirm = inherit)) {
                Text(str("overlay.osd.inherit"))
            }
        } else Text(str("overlay.osd.inheriting"), color = MaterialTheme.colorScheme.onSurfaceVariant)
    } else if (InGameOverlay.hasGameOsdOverride(contextGameKey)) {
        Text(str("overlay.osd.overridden"), color = MaterialTheme.colorScheme.onSurfaceVariant)
    }
    if (mode == InGameOverlay.OsdMode.Custom) {
        var custom by remember(gameKey) { mutableStateOf(ConfigStore.resolveForGame(gameKey)) }
        fun update(next: Settings) {
            ConfigStore.save(scope, gameKey, next, custom)
            custom = next
            if (InGameOverlay.settingsScope.value == scope && InGameOverlay.currentSerial.value == gameKey)
                InGameOverlay.settingsState.value = next
            InGameOverlay.applyStoredOsdMode()
        }
        Text(str("overlay.osd.customHelp"), color = MaterialTheme.colorScheme.onSurfaceVariant)
        ToggleRow(str("overlay.toggle.fps"), custom.osdShowFps) { update(custom.copy(osdShowFps = it)) }
        ToggleRow(str("overlay.osd.speed"), custom.osdShowSpeed) { update(custom.copy(osdShowSpeed = it)) }
        ToggleRow(str("overlay.osd.cpu"), custom.osdShowCpu) { update(custom.copy(osdShowCpu = it)) }
        ToggleRow(str("overlay.toggle.gpuPrimitives"), custom.osdShowGpu) { update(custom.copy(osdShowGpu = it)) }
        ToggleRow(str("overlay.osd.devices"), custom.osdShowGsStats) { update(custom.copy(osdShowGsStats = it)) }
        ToggleRow(str("overlay.toggle.frameTimes"), custom.osdShowFrameTimes) { update(custom.copy(osdShowFrameTimes = it)) }
        ToggleRow(str("overlay.toggle.internalResolution"), custom.osdShowResolution) { update(custom.copy(osdShowResolution = it)) }
        ToggleRow(str("overlay.osd.renderer"), custom.osdShowHardwareInfo) { update(custom.copy(osdShowHardwareInfo = it)) }
        ToggleRow(str("overlay.toggle.deviceUsage"), custom.osdShowHostUsage) { update(custom.copy(osdShowHostUsage = it)) }
        ToggleRow(str("overlay.toggle.controlInputs"), custom.osdShowInputs) { update(custom.copy(osdShowInputs = it)) }
    }
}
