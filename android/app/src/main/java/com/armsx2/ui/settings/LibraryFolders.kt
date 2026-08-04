package com.armsx2.ui.settings

import android.content.Intent
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
import com.armsx2.i18n.I18n
import com.armsx2.i18n.str
import com.armsx2.runtime.MainActivityRuntime

/**
 * Game-library folders. Their URI identities are stored in app preferences; only optional raw
 * POSIX roots are mirrored to `settings.toml [library].folders`.
 *
 * Folder identity stays as a persisted ACTION_OPEN_DOCUMENT_TREE URI. The library enumerates it
 * through SAF and the launch path leases seekable descriptors to the native core, so Android 11+
 * never needs a guessed `/storage/...` path or broad all-files access.
 *
 * Kept as its own section composable so it can be dropped into any tab — it is currently hosted by
 * [Ps1LibraryTab] and reachable from the settings hub's Library chip.
 */
@Composable
fun LibraryFoldersSection() {
    val context = LocalContext.current
    val editor = rememberPs1SettingsEditor()
    val folders = MainActivityRuntime.romsDirs.value
    val recursive = editor.value.recursiveFolders

    val picker = rememberLauncherForActivityResult(ActivityResultContracts.OpenDocumentTree()) { uri ->
        if (uri != null) {
            val granted = runCatching {
                context.contentResolver.takePersistableUriPermission(
                    uri,
                    Intent.FLAG_GRANT_READ_URI_PERMISSION,
                )
                true
            }.getOrElse {
                context.contentResolver.persistedUriPermissions.any { permission ->
                    permission.isReadPermission && permission.uri == uri
                }
            }
            if (granted) {
                MainActivityRuntime.setRomsDirs((folders + uri.toString()).distinct())
                com.armsx2.core.Ps1Library.rescan.intValue++
            } else {
                Toast.makeText(
                    context,
                    I18n.get("library.noPath"),
                    Toast.LENGTH_LONG,
                ).show()
            }
        }
    }

    Column(modifier = Modifier.fillMaxWidth()) {
        Text(
            str("library.intro"),
            color = MaterialTheme.colorScheme.onSurfaceVariant,
            fontSize = 14.sp,
            modifier = Modifier.padding(bottom = 8.dp),
        )

        Ps1ActionRow(
            label = str("library.addFolder.label"),
            controllerId = "ps1.library.add",
            description = str("library.addFolder.description"),
        ) {
            runCatching { picker.launch(null) }
        }
        SettingsDivider()

        if (folders.isEmpty()) {
            HelpText(str("library.empty"))
        } else {
            folders.forEach { path ->
                Ps1InfoRow(label = str("library.folder"), value = path)
                ToggleRow(
                    str("library.scanSubfolders"),
                    recursive.contains(path),
                    description = path,
                    idSuffix = path,
                ) { on ->
                    editor.update { s ->
                        val next = if (on) (s.recursiveFolders + path).distinct()
                        else s.recursiveFolders.filterNot { it == path }
                        s.copy(recursiveFolders = next)
                    }
                }
                Ps1ActionRow(
                    label = str("library.remove"),
                    controllerId = "ps1.library.remove:$path",
                ) {
                    MainActivityRuntime.setRomsDirs(folders.filterNot { it == path })
                    com.armsx2.core.Ps1Library.rescan.intValue++
                    editor.update { s ->
                        s.copy(recursiveFolders = s.recursiveFolders.filterNot { it == path })
                    }
                }
                SettingsDivider()
            }
        }
    }
}

/** Library tab — currently just the folder manager. */
@Composable
fun Ps1LibraryTab() {
    val scroll = settingsScrollState()
    ControllerAutoScroll(scroll)
    LibraryFoldersSection()
}
