package com.armsx2.ui.settings

import androidx.compose.foundation.background
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.armsx2.config.ConfigStore
import com.armsx2.config.Ps1GameSettings
import com.armsx2.config.Ps1SettingsStore
import com.armsx2.config.Settings
import com.armsx2.i18n.I18n
import com.armsx2.i18n.str
import com.armsx2.runtime.MainActivityRuntime
import com.armsx2.ui.InGameOverlay

private val LocalChangedLabels = staticCompositionLocalOf { emptySet<String>() }

/** One snapshot per visible settings pane; rows do not reread files individually. */
@Composable
internal fun ChangedSettingsProvider(gameKey: String?, content: @Composable () -> Unit) {
    val context = LocalContext.current.applicationContext
    val coreVersion = Ps1SettingsStore.version.intValue
    val gameVersion = Ps1GameSettings.version.intValue
    val hostVersion = ConfigStore.version.intValue
    val preferences = MainActivityRuntime.prefs
    var preferenceVersion by remember(preferences) { mutableIntStateOf(0) }
    DisposableEffect(preferences) {
        val listener = android.content.SharedPreferences.OnSharedPreferenceChangeListener { _, _ -> preferenceVersion++ }
        preferences.registerOnSharedPreferenceChangeListener(listener)
        onDispose { preferences.unregisterOnSharedPreferenceChangeListener(listener) }
    }
    val core = remember(gameKey, coreVersion, gameVersion) { Ps1SettingsStore.changedFromDefaults(Ps1SettingsStore.load(context, gameKey)) }
    val host = remember(gameKey, hostVersion) { Settings.diff(Settings(), ConfigStore.resolveForGame(gameKey)) }
    val language = I18n.current
    val osd = InGameOverlay.configuredOsdMode(if (gameKey == null) com.armsx2.config.SettingsScope.Global else com.armsx2.config.SettingsScope.Game, gameKey)
    val labels = remember(core, host, language, preferenceVersion, osd) {
        changedCoreLabels(core) + HOST_FIELDS.filterValues(host::has).keys.map(I18n::get) + buildSet {
            if (preferences.getBoolean("ui.sustainedPerf", false)) add(I18n.get("renderer.sustainedPerf.label"))
            if (preferences.getBoolean("ui.adpf", false)) add(I18n.get("renderer.adpf.label"))
            if (osd != InGameOverlay.OsdMode.Off) add(I18n.get("renderer.debugPanel.label"))
        }
    }
    CompositionLocalProvider(LocalChangedLabels provides labels, content = content)
}

private val HOST_FIELDS = mapOf(
    "renderer.shaderChain.label" to "shaderChainEnabled",
    "renderer.shaderChain.preset.label" to "shaderChainPreset",
    "renderer.shaderChain.params.label" to "shaderChainParams",
    "renderer.orientation.label" to "orientation",
    "renderer.outputScale.label" to "hwScaler",
    "renderer.screenRes.label" to "screenResOverride",
    "renderer.lowLatency.label" to "vsyncQueueSize",
    "renderer.affinity.label" to "affinityMode",
    "overlay.toggle.fps" to "osdShowFps",
    "overlay.osd.speed" to "osdShowSpeed",
    "overlay.osd.cpu" to "osdShowCpu",
    "overlay.toggle.gpuPrimitives" to "osdShowGpu",
    "overlay.osd.devices" to "osdShowGsStats",
    "overlay.toggle.frameTimes" to "osdShowFrameTimes",
    "overlay.toggle.internalResolution" to "osdShowResolution",
    "overlay.osd.renderer" to "osdShowHardwareInfo",
    "overlay.toggle.deviceUsage" to "osdShowHostUsage",
    "overlay.toggle.controlInputs" to "osdShowInputs",
)

@Composable
internal fun ChangedSettingMarker(label: String) {
    if (label !in LocalChangedLabels.current) return
    val hint = str("settings.changedFromDefault.help")
    Row(verticalAlignment = Alignment.CenterVertically,
        modifier = Modifier.padding(start = 6.dp).semantics { contentDescription = hint }) {
        Box(Modifier.size(6.dp).background(MaterialTheme.colorScheme.primary, CircleShape))
        Spacer(Modifier.width(4.dp))
        Text(str("settings.changedFromDefault"), color = MaterialTheme.colorScheme.primary,
            fontSize = 11.sp, fontWeight = FontWeight.SemiBold, maxLines = 1)
    }
}
