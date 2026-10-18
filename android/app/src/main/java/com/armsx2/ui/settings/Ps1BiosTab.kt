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

/** Model-based selection. Importing adds a dump without forcing it over the model choice. */
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
                editor.update { it.copy(biosSearchPath = imported.parent ?: it.biosSearchPath) }
                Toast.makeText(context, I18n.get("bios.import.ok"), Toast.LENGTH_SHORT).show()
            } else {
                Toast.makeText(context, I18n.get("bios.import.failed"), Toast.LENGTH_LONG).show()
            }
        }
    }

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

        Ps1ActionRow(
            label = str("bios.import.label"),
            controllerId = "ps1.bios.import",
            description = str("bios.import.description"),
        ) {
            picker.launch(arrayOf("*/*"))
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
