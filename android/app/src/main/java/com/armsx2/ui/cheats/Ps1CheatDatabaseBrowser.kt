package com.armsx2.ui.cheats

import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.unit.dp
import com.armsx2.cheats.Ps1CheatCatalogue
import com.armsx2.cheats.Ps1CheatDatabase
import com.armsx2.cheats.Ps1CheatDatabaseIndex
import com.armsx2.cheats.Ps1CheatDatabaseItem
import com.armsx2.i18n.str
import com.armsx2.ui.common.GlassPanel
import com.armsx2.ui.common.SectionTitle
import com.armsx2.ui.home.LibraryKeyboard
import com.armsx2.ui.settings.Ps1ActionRow
import com.armsx2.ui.settings.Ps1InfoRow
import com.armsx2.ui.settings.SettingsDivider
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext

/** Always-available offline browser. Never writes files, preferences or core cheats. */
@Composable
internal fun Ps1CheatDatabaseBrowser() {
    val context = LocalContext.current.applicationContext
    var database by remember { mutableStateOf<Ps1CheatDatabaseIndex?>(null) }
    var failed by remember { mutableStateOf(false) }
    var retry by remember { mutableIntStateOf(0) }
    var query by remember { mutableStateOf("") }
    var selected by remember { mutableStateOf<Ps1CheatDatabaseItem?>(null) }
    var preview by remember { mutableStateOf<List<String>?>(null) }
    var previewFailed by remember { mutableStateOf(false) }
    var limit by remember(query, selected) { mutableIntStateOf(30) }
    val searchHint = str("cheats.database.search")

    LaunchedEffect(retry) {
        failed = false
        val result = withContext(Dispatchers.IO) { runCatching { Ps1CheatDatabase.index(context) } }
        database = result.getOrNull()
        failed = result.isFailure
    }
    LaunchedEffect(selected, retry) {
        preview = null
        previewFailed = false
        val item = selected ?: return@LaunchedEffect
        val result = withContext(Dispatchers.IO) {
            runCatching { Ps1CheatCatalogue.merge("", Ps1CheatDatabase.catalogue(context, item)).addedNames }
        }
        preview = result.getOrNull()
        previewFailed = result.isFailure
    }

    Column(Modifier.fillMaxWidth()) {
        GlassPanel(Modifier.fillMaxWidth()) {
            Column {
                SectionTitle(str("cheats.database.start.title"), str("cheats.database.start.description"))
                Ps1InfoRow(str("cheats.database.browse"),
                    if (database != null) str("cheats.database.description").format(database!!.items.size)
                    else str("cheats.database.loading"))
                database?.let { index ->
                    Ps1InfoRow(str("cheats.database.coverage"),
                        str("cheats.database.coverage.description").format(index.coveredGameSerialCount, index.knownGameSerialCount))
                }
            }
        }
        Spacer(Modifier.height(10.dp))

        if (failed || previewFailed) {
            Text(str("cheats.database.start.failed"), color = MaterialTheme.colorScheme.error)
            Ps1ActionRow(str("cheats.database.start.retry"), controllerId = "ps1.cheats.start.retry") { retry++ }
        }
        val item = selected
        if (item != null) {
            Ps1ActionRow(str("cheats.database.start.back"), controllerId = "ps1.cheats.start.back") { selected = null }
            Ps1InfoRow(item.title, str("cheats.database.count").format(item.cheatCount, item.omittedCount))
            Text(str("cheats.database.start.preview"), color = MaterialTheme.colorScheme.onSurfaceVariant)
            if (preview == null && !previewFailed) Text(str("cheats.database.loading"))
            preview.orEmpty().take(limit).forEach { name ->
                Ps1InfoRow(name, str("cheats.database.start.disabled"))
                SettingsDivider()
            }
            if (preview.orEmpty().size > limit) {
                Ps1ActionRow(str("cheats.database.start.moreCheats"), controllerId = "ps1.cheats.start.moreCheats") { limit += 30 }
            }
        } else {
            Ps1ActionRow(str("cheats.database.search"), controllerId = "ps1.cheats.start.search",
                description = query.ifBlank { searchHint }) {
                LibraryKeyboard.open(initial = query, onChange = { query = it }, placeholder = searchHint)
            }
            if (query.isNotBlank()) {
                Ps1ActionRow(str("cheats.database.clearSearch"), controllerId = "ps1.cheats.start.clear") { query = "" }
            }
            val results = remember(database, query) { database?.search(query).orEmpty() }
            if (database != null && results.isEmpty()) Text(str("cheats.database.noResults"))
            results.take(limit).forEach { entry ->
                Ps1ActionRow(entry.title, controllerId = "ps1.cheats.start.${entry.id}",
                    description = str("cheats.database.count").format(entry.cheatCount, entry.omittedCount)) { selected = entry }
                SettingsDivider()
            }
            if (results.size > limit) {
                Ps1ActionRow(str("cheats.database.more"), controllerId = "ps1.cheats.start.more") { limit += 30 }
            }
        }
        Ps1InfoRow(str("cheats.database.source"), "Libretro contributors · CC BY-SA 4.0",
            "https://github.com/libretro/libretro-database · https://creativecommons.org/licenses/by-sa/4.0/")
    }
}
