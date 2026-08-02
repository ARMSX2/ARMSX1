package com.armsx2.ui.settings

import android.widget.Toast
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
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
import java.io.File

/**
 * BIOS tab — the PS1 core's `[bios]` section (`search_path`, `preferred_model`, `override_file`).
 *
 * `preferred_model` is what the core's BIOS auto-selector matches against when it scans
 * `search_path`; the list mirrors `g_models_text` in `frontend/config.c`. `override_file` wins over
 * the whole search when it is set, so the picker below imports a dump into app storage (the core's
 * pref path — see [Ps1Config]) and points the key straight at it.
 */
@Composable
fun Ps1BiosTab() {
    val context = LocalContext.current
    val editor = rememberPs1SettingsEditor()
    val s = editor.value
    val scroll = settingsScrollState()
    ControllerAutoScroll(scroll)

    val picker = rememberLauncherForActivityResult(ActivityResultContracts.OpenDocument()) { uri ->
        if (uri != null) {
            val imported = Ps1Config.importBios(context, uri)
            if (imported != null) {
                editor.update { it.copy(biosOverrideFile = imported.absolutePath) }
                Toast.makeText(context, I18n.get("bios.import.ok"), Toast.LENGTH_SHORT).show()
            } else {
                Toast.makeText(context, I18n.get("bios.import.failed"), Toast.LENGTH_LONG).show()
            }
        }
    }

    val overrideFile = s.biosOverrideFile.takeIf { it.isNotBlank() }?.let { File(it) }
    val overrideLabel = overrideFile?.let { f ->
        if (f.isFile) str("bios.forced.size").format(f.name, f.length() / 1024)
        else str("bios.forced.missing").format(f.name)
    } ?: str("bios.forced.none")

    Column(modifier = Modifier.fillMaxWidth()) {
        Text(
            str("bios.intro"),
            color = MaterialTheme.colorScheme.onSurfaceVariant,
            fontSize = 14.sp,
            modifier = Modifier.padding(bottom = 8.dp),
        )

        SegmentedGridRow(
            label = str("bios.model.label"),
            options = Ps1Settings.MODELS,
            selectedIndex = Ps1Settings.MODELS.indexOf(s.preferredModel).coerceAtLeast(0),
            columns = 3,
            description = str("bios.model.description"),
            onChange = { idx -> editor.update { it.copy(preferredModel = Ps1Settings.MODELS[idx]) } },
        )
        SettingsDivider()

        Ps1InfoRow(
            label = str("bios.forced.label"),
            value = overrideLabel,
            description = str("bios.forced.description"),
        )
        Ps1ActionRow(
            label = str("bios.import.label"),
            controllerId = "ps1.bios.import",
            description = str("bios.import.description"),
        ) {
            picker.launch(arrayOf("*/*"))
        }
        if (overrideFile != null) {
            Ps1ActionRow(
                label = str("bios.clearForced.label"),
                controllerId = "ps1.bios.clearOverride",
                description = str("bios.clearForced.description"),
            ) {
                editor.update { it.copy(biosOverrideFile = "") }
            }
        }
        SettingsDivider()

        Ps1InfoRow(
            label = str("bios.searchFolder.label"),
            value = s.biosSearchPath,
            description = str("bios.searchFolder.description").format(context.filesDir.absolutePath),
        )
        Ps1InfoRow(
            label = str("bios.bundled.label"),
            value = if (Ps1Config.bundledBiosFile(context).isFile) str("bios.bundled.present")
            else str("bios.bundled.absent"),
            description = str("bios.bundled.description"),
        )
        SettingsDivider()

        Ps1ActionRow(
            label = str("bios.reset.label"),
            controllerId = "ps1.bios.reset",
            description = str("bios.reset.description"),
        ) {
            val d = Ps1Settings()
            editor.update {
                it.copy(
                    preferredModel = d.preferredModel,
                    biosOverrideFile = d.biosOverrideFile,
                    biosSearchPath = d.biosSearchPath,
                )
            }
        }
    }
}
