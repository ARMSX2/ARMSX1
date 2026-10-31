package com.armsx2.ui.settings

import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.remember
import androidx.compose.ui.Modifier
import com.armsx2.config.ConfigStore
import com.armsx2.config.Ps1SettingsStore
import com.armsx2.config.Settings
import com.armsx2.config.SettingsScope
import com.armsx2.core.Ps1Shaders
import com.armsx2.ui.InGameOverlay

/** The visible settings scope owns edits; the running game's scope owns live application. */
@Composable
internal fun ScopedShaderSettings(gameKey: String? = LocalPs1SettingsScope.current?.gameKey) {
    val revision = ConfigStore.version.intValue
    val settings = remember(gameKey, revision) { ConfigStore.resolveForGame(gameKey) }
    val shaderKeys = setOf("shaderChainEnabled", "shaderChainPreset", "shaderChainParams")
    val pinned = gameKey != null && remember(gameKey, revision) {
        ConfigStore.loadOverrides(gameKey)?.let { overrides -> shaderKeys.any(overrides::has) } == true
    }
    fun refresh() {
        val overlayKey = if (InGameOverlay.settingsScope.value == SettingsScope.Game) InGameOverlay.currentSerial.value else null
        InGameOverlay.settingsState.value = ConfigStore.resolveForGame(overlayKey)
        Ps1Shaders.pushForGame(Ps1SettingsStore.activeGameKey)
    }
    fun update(edit: (Settings) -> Settings) {
        ConfigStore.update(if (gameKey == null) SettingsScope.Global else SettingsScope.Game, gameKey, edit)
        refresh()
    }
    Text(com.armsx2.i18n.str(when {
        gameKey == null -> "renderer.shaderChain.scope.global"
        pinned -> "renderer.shaderChain.scope.game"
        else -> "renderer.shaderChain.scope.inherited"
    }), style = androidx.compose.material3.MaterialTheme.typography.bodySmall,
        color = androidx.compose.material3.MaterialTheme.colorScheme.onSurfaceVariant)
    com.armsx2.ui.common.ShaderChainSection(
        enabled = settings.shaderChainEnabled,
        preset = settings.shaderChainPreset,
        params = settings.shaderChainParams,
        onEnabledChange = { value -> update { it.copy(shaderChainEnabled = value) } },
        onPresetChange = { value -> update { it.copy(shaderChainPreset = value) } },
        onParamsChange = { value -> update { it.copy(shaderChainParams = value) } },
    )
    if (pinned) {
        val inherit = {
            ConfigStore.loadOverrides(gameKey)?.let { overrides ->
                shaderKeys.forEach(overrides::remove)
                if (overrides.length() == 0) ConfigStore.clearOverrides(gameKey)
                else ConfigStore.saveOverrides(gameKey, overrides)
            }
            refresh()
        }
        TextButton(onClick = inherit, modifier = Modifier.controllerFocusable("settings.shaders.inherit", onConfirm = inherit)) {
            Text(com.armsx2.i18n.str("renderer.shaderChain.inherit"))
        }
    }
}
