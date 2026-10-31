package com.armsx2.ui.settings

import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import com.armsx2.config.ConfigStore
import com.armsx2.i18n.str
import com.armsx2.runtime.MainActivityRuntime
import com.armsx2.ui.InGameOverlay
import org.json.JSONObject

/** Activity orientation, not the core's 0/90/180/270-degree image rotation. */
@Composable
internal fun GameplayOrientationSettings() {
    val gameKey = LocalPs1SettingsScope.current?.gameKey
    val host = InGameOverlay.settingsState.value
    // An explicit choice may equal global, so keep the pin observable even in that case.
    var revision by remember(gameKey) { mutableIntStateOf(0) }
    val overrides = remember(gameKey, host.orientation, revision) {
        gameKey?.let(ConfigStore::loadOverrides)
    }
    val inherited = gameKey != null && overrides?.has("orientation") != true
    val offset = if (gameKey == null) 0 else 1
    val options = listOf(
        str("renderer.orientation.device"),
        str("renderer.orientation.landscape"),
        str("renderer.orientation.portrait"),
        str("renderer.orientation.autoRotate"),
    )
    SegmentedRow(
        label = str("renderer.orientation.label"),
        options = if (gameKey == null) options
            else listOf(str("renderer.orientation.global")) + options,
        selectedIndex = if (inherited) 0 else host.orientation.coerceIn(0, 3) + offset,
        description = str("renderer.orientation.description"),
        onChange = { index ->
            if (gameKey == null) {
                InGameOverlay.saveSettings(host.copy(orientation = index.coerceIn(0, 3)))
            } else {
                // Host settings are not settings.toml keys. Preserve every unrelated override,
                // and allow an explicit pin even when it happens to match the global choice.
                val next = ConfigStore.loadOverrides(gameKey) ?: JSONObject()
                if (index == 0) next.remove("orientation")
                else next.put("orientation", (index - offset).coerceIn(0, 3))
                if (next.length() == 0) ConfigStore.clearOverrides(gameKey)
                else ConfigStore.saveOverrides(gameKey, next)
                InGameOverlay.settingsState.value = ConfigStore.resolveForGame(gameKey)
            }
            revision++
            MainActivityRuntime.instance?.applyEmulationOrientation()
        },
    )
}
