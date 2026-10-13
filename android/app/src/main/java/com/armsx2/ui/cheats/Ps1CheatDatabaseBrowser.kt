package com.armsx2.ui.cheats

import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
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
import com.armsx2.ui.common.StatusChip
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
    var showDetails by remember { mutableStateOf(false) }
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
                SectionTitle(str("cheats.database.start.title"), str("cheats.ui.browserHint"))
                Spacer(Modifier.height(8.dp))
                if (database != null) StatusChip(str("cheats.ui.catalogues").format(database!!.items.size))
                else Text(str("cheats.database.loading"))
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
            SectionTitle(item.title, catalogueCount(item))
            Text(str("cheats.database.start.preview"), color = MaterialTheme.colorScheme.onSurfaceVariant)
            if (preview == null && !previewFailed) Text(str("cheats.database.loading"))
            preview.orEmpty().take(limit).forEach { name ->
                Text(name, color = MaterialTheme.colorScheme.onSurface,
                    modifier = Modifier.padding(horizontal = 12.dp, vertical = 10.dp))
                SettingsDivider()
            }
            if (preview.orEmpty().size > limit) {
                Ps1ActionRow(str("cheats.database.start.moreCheats"), controllerId = "ps1.cheats.start.moreCheats") { limit += 30 }
            }
        } else {
            CheatSearchBar(query, searchHint, "ps1.cheats.start.search", onClear = { query = "" }) {
                LibraryKeyboard.open(initial = query, onChange = { query = it }, placeholder = searchHint)
            }
            val results = remember(database, query) { database?.search(query).orEmpty() }
            if (database != null && results.isEmpty()) Text(str("cheats.database.noResults"))
            results.take(limit).forEach { entry ->
                Ps1ActionRow(entry.title, controllerId = "ps1.cheats.start.${entry.id}",
                    description = catalogueCount(entry)) { selected = entry }
            }
            if (results.size > limit) {
                Ps1ActionRow(str("cheats.database.more"), controllerId = "ps1.cheats.start.more") { limit += 30 }
            }
        }
        CheatDatabaseAttribution()
        Ps1ActionRow(str(if (showDetails) "cheats.ui.details.close" else "cheats.ui.details"),
            controllerId = "ps1.cheats.start.details") { showDetails = !showDetails }
        if (showDetails) {
            database?.let { index ->
                Ps1InfoRow(str("cheats.database.coverage"),
                    str("cheats.database.coverage.description").format(index.coveredGameSerialCount, index.knownGameSerialCount))
            }
            Ps1InfoRow(str("cheats.database.source"), "Libretro contributors · CC BY-SA 4.0",
                "https://github.com/libretro/libretro-database · https://creativecommons.org/licenses/by-sa/4.0/")
        }
    }
}
