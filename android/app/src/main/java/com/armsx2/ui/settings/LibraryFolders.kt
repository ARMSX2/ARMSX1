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
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.armsx2.core.Ps1Folders
import com.armsx2.core.Ps1Storage
import com.armsx2.i18n.I18n
import com.armsx2.i18n.str

/**
 * Game-library folders — `settings.toml [library].folders` / `recursive_folders`.
 *
 * The PS1 core opens games by absolute filesystem path (no SAF), so a folder is picked through
 * ACTION_OPEN_DOCUMENT_TREE purely to derive a `/storage/...` path, and the app needs all-files
 * access for that path to be readable. [Ps1Folders] owns the canonical list (prefs + mirrored into
 * the TOML); the per-folder recursion flag is a second list in the same TOML section, so it is
 * written here through [Ps1SettingsEditor].
 *
 * Kept as its own section composable so it can be dropped into any tab — it is currently hosted by
 * [Ps1LibraryTab] and reachable from the settings hub's Library chip.
 */
@Composable
fun LibraryFoldersSection() {
    val context = LocalContext.current
    val editor = rememberPs1SettingsEditor()
    val folders = Ps1Folders.folders.value
    val recursive = editor.value.recursiveFolders

    // Ps1Folders is a process-wide singleton that may not have been hydrated yet (nothing else in
    // the app reads it), so pull it from prefs the first time this section composes.
    LaunchedEffect(Unit) { runCatching { Ps1Folders.load() } }

    val picker = rememberLauncherForActivityResult(
        ActivityResultContracts.StartActivityForResult(),
    ) { result ->
        val uri = result.data?.data
        val path = Ps1Storage.resolveTreeUriToPosix(uri?.toString())
        if (path != null) {
            Ps1Folders.add(path)
            // add() rewrote settings.toml behind us — resync this tab's snapshot.
            editor.reload()
        } else if (uri != null) {
            Toast.makeText(
                context,
                I18n.get("library.noPath"),
                Toast.LENGTH_LONG,
            ).show()
        }
    }

    Column(modifier = Modifier.fillMaxWidth()) {
        Text(
            str("library.intro"),
            color = MaterialTheme.colorScheme.onSurfaceVariant,
            fontSize = 14.sp,
            modifier = Modifier.padding(bottom = 8.dp),
        )

        if (!Ps1Storage.hasAllFilesAccess()) {
            Ps1ActionRow(
                label = str("library.allFiles.label"),
                controllerId = "ps1.library.allFiles",
                description = str("library.allFiles.description"),
            ) {
                runCatching { context.startActivity(Ps1Storage.allFilesAccessIntent(context)) }
            }
            SettingsDivider()
        }

        Ps1ActionRow(
            label = str("library.addFolder.label"),
            controllerId = "ps1.library.add",
            description = str("library.addFolder.description"),
        ) {
            runCatching { picker.launch(Ps1Storage.pickFolderIntent()) }
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
                    // Ps1Folders.remove rewrites the TOML itself; update() re-reads from disk, so
                    // dropping the recursion flag afterwards preserves the removal.
                    Ps1Folders.remove(path)
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
