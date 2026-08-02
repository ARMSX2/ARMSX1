package com.armsx2.ui.settings

import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.armsx2.config.Ps1Settings
import com.armsx2.core.Ps1Config
import com.armsx2.i18n.I18n
import com.armsx2.i18n.str

/** Level names for `[runtime].log_level`, index-aligned with the core's `enum { LOG_TRACE … }`.
 *  Resolved on every read (not stored) so a live language switch relabels the slider. */
private val LOG_LEVEL_KEYS = listOf(
    "advanced.logLevel.trace",
    "advanced.logLevel.debug",
    "advanced.logLevel.info",
    "advanced.logLevel.warn",
    "advanced.logLevel.error",
    "advanced.logLevel.fatal",
)

/**
 * Advanced tab — the PS1 core's `[runtime]` logging switches and the `[paths]` launch defaults.
 *
 * This replaces the PS2 build's Fixes tab wholesale. None of what lived there (GameDB game fixes,
 * VU/EE recompiler toggles, upscaling hacks, half-pixel offset, CPU sprite render …) has a
 * PlayStation equivalent — the ARMSX1 core ships no hack surface at all, so the honest content of
 * an "Advanced" page is diagnostics.
 *
 * The two `[paths]` entries are shown read-only: they are launch defaults the core resolves at
 * startup and nothing in the launcher writes them today.
 */
@Composable
fun Ps1AdvancedTab() {
    val context = LocalContext.current
    val editor = rememberPs1SettingsEditor()
    val s = editor.value
    val scroll = settingsScrollState()
    ControllerAutoScroll(scroll)

    Column(modifier = Modifier.fillMaxWidth()) {
        Text(
            str("advanced.intro"),
            color = MaterialTheme.colorScheme.onSurfaceVariant,
            fontSize = 14.sp,
            modifier = Modifier.padding(bottom = 8.dp),
        )

        // logging_enabled and quiet are ONE switch in the core, not two: main.cpp reads
        // logging_enabled first and derives `quiet = !logging_enabled`, only falling back to the
        // quiet key when logging_enabled is absent — and this writer always emits both. So each
        // row here sets the pair; writing just one of them would produce a toggle that flips in
        // the UI and changes nothing at launch.
        ToggleRow(
            str("advanced.logging.label"),
            s.loggingEnabled,
            description = str("advanced.logging.description"),
        ) { v -> editor.update { it.copy(loggingEnabled = v, quiet = !v) } }
        SettingsDivider()

        val defaultLevel = Ps1Settings().logLevel
        val resetLevel: (() -> Unit)? =
            if (s.logLevel != defaultLevel) {
                { editor.update { it.copy(logLevel = defaultLevel) } }
            } else {
                null
            }
        IntSliderRow(
            label = str("advanced.logLevel.label"),
            value = s.logLevel.coerceIn(0, 5),
            min = 0,
            max = 5,
            description = str("advanced.logLevel.description"),
            valueFormatter = { level ->
                LOG_LEVEL_KEYS.getOrNull(level)?.let { I18n.get(it) } ?: level.toString()
            },
            onReset = resetLevel,
            onChange = { v -> editor.update { it.copy(logLevel = v) } },
        )
        SettingsDivider()

        ToggleRow(
            str("advanced.quiet.label"),
            s.quiet,
            description = str("advanced.quiet.description"),
        ) { v -> editor.update { it.copy(quiet = v, loggingEnabled = !v) } }
        SettingsDivider()

        Ps1InfoRow(
            label = str("advanced.expansionRom.label"),
            value = s.expansionRom,
            description = str("advanced.expansionRom.description"),
        )
        Ps1InfoRow(
            label = str("advanced.defaultExe.label"),
            value = s.defaultPsxExe,
            description = str("advanced.defaultExe.description"),
        )
        Ps1InfoRow(
            label = str("advanced.settingsFile.label"),
            value = Ps1Config.settingsFile(context).absolutePath,
            description = str("advanced.settingsFile.description"),
        )
        SettingsDivider()

        Ps1ActionRow(
            label = str("advanced.reset.label"),
            controllerId = "ps1.advanced.reset",
            description = str("advanced.reset.description"),
        ) {
            val d = Ps1Settings()
            editor.update {
                it.copy(
                    loggingEnabled = d.loggingEnabled,
                    logLevel = d.logLevel,
                    quiet = !d.loggingEnabled,
                )
            }
        }
    }
}
