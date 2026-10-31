package com.armsx2.ui.achievements

import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.produceState
import androidx.compose.runtime.remember
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import com.armsx2.GameInfo
import com.armsx2.PlayTime
import com.armsx2.RaLibrary
import com.armsx2.achievements.RaLibraryBrowser
import com.armsx2.data.library.GameLibraryRepository
import com.armsx2.i18n.str
import com.armsx2.ui.common.EmptyState
import com.armsx2.ui.common.GameCoverArt
import com.armsx2.ui.common.SectionTitle
import com.armsx2.ui.settings.controllerFocusable
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext

private data class LibraryGameProgress(val game: GameInfo, val title: String, val progress: PlayTime.Progress)
private data class LibraryProgressSnapshot(
    val account: String, val games: List<LibraryGameProgress> = emptyList(), val total: Int = 0,
    val loading: Boolean = true, val failed: Boolean = false,
)

/** The game list reads saved sync results; selecting a game loads its individual achievements. */
@Composable
internal fun LibraryAchievementBrowser(account: String, modifier: Modifier = Modifier) {
    val context = LocalContext.current.applicationContext
    val revision = PlayTime.revision.value
    val syncing = RaLibrary.syncing.value
    val customNames = com.armsx2.CustomNames.version.intValue
    val forceEnglish = com.armsx2.EnglishTitles.enabled.value
    val snapshot by produceState(LibraryProgressSnapshot(account), account, revision, syncing, customNames, forceEnglish) {
        if (!syncing) value = withContext(Dispatchers.IO) {
            runCatching {
                val games = GameLibraryRepository(context).loadCached().games.distinctBy { it.uri.toString() }
                val rows = games.mapNotNull { game ->
                    RaLibrary.syncedProgressFor(game, account)?.let {
                        LibraryGameProgress(game, game.displayTitle(forceEnglish), it)
                    }
                }
                LibraryProgressSnapshot(account, rows, games.size, loading = false)
            }.getOrElse { LibraryProgressSnapshot(account, loading = false, failed = true) }
        }
    }
    // Never retain another account's rows during an asynchronous reload.
    val rows = snapshot.takeIf { it.account == account }?.games.orEmpty()
    var query by rememberSaveable(account) { mutableStateOf("") }
    var page by rememberSaveable(account, query) { mutableIntStateOf(0) }
    var selected by remember(account) { mutableStateOf<LibraryGameProgress?>(null) }
    val filtered = remember(rows, query) {
        RaLibraryBrowser.select(rows, query, { it.title }, { it.game.serial }, { it.progress.total })
    }
    val currentPage = RaLibraryBrowser.safePage(page, filtered.size)

    selected?.let { row ->
        LibraryGameAchievements(row.game, row.title, account, onBack = { selected = null })
        return
    }

    Column(modifier, verticalArrangement = Arrangement.spacedBy(10.dp)) {
        SectionTitle(str("ra.library.games"), "${rows.size} / ${snapshot.total}")
        Text(str("ra.library.games.desc"), style = MaterialTheme.typography.bodySmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant)
        OutlinedTextField(
            value = query, onValueChange = { query = it }, singleLine = true,
            label = { Text(str("ra.library.search")) },
            modifier = Modifier.fillMaxWidth(), shape = RoundedCornerShape(18.dp),
        )
        when {
            snapshot.account != account || snapshot.loading -> CircularProgressIndicator(Modifier.size(24.dp))
            snapshot.failed -> EmptyState(str("ra.library.games.error"), str("ra.library.games.retry"))
            filtered.isEmpty() -> EmptyState(str("ra.library.games.empty"),
                str(if (query.isNotBlank()) "ra.library.games.noSearch" else "ra.library.games.sync"))
            else -> {
                RaLibraryBrowser.page(filtered, currentPage).forEach { row ->
                    Surface(
                        shape = RoundedCornerShape(18.dp), color = MaterialTheme.colorScheme.surfaceVariant,
                        modifier = Modifier.fillMaxWidth().clickable { selected = row }.controllerFocusable(
                            "ra.library.game.${row.game.uri}", onConfirm = { selected = row },
                        ),
                    ) {
                        Row(Modifier.padding(12.dp), horizontalArrangement = Arrangement.spacedBy(12.dp),
                            verticalAlignment = Alignment.CenterVertically) {
                            GameCoverArt(row.game, Modifier.size(58.dp))
                            Column(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(4.dp)) {
                                Text(row.title, fontWeight = FontWeight.Bold, maxLines = 2, overflow = TextOverflow.Ellipsis)
                                row.game.serial?.takeIf(String::isNotBlank)?.let {
                                    Text(it, style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
                                }
                                Text("${row.progress.softcore} / ${row.progress.total} SC · ${row.progress.hardcore} / ${row.progress.total} HC",
                                    style = MaterialTheme.typography.bodySmall)
                            }
                        }
                    }
                }
                if (filtered.size > RaLibraryBrowser.PAGE_SIZE) Row(
                    Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.SpaceBetween,
                    verticalAlignment = Alignment.CenterVertically,
                ) {
                    TextButton(onClick = { page = currentPage - 1 }, enabled = currentPage > 0,
                        modifier = Modifier.controllerFocusable("ra.library.previous", onConfirm = {
                            if (currentPage > 0) page = currentPage - 1
                        })) { Text(str("ra.library.previous")) }
                    Text("${currentPage + 1} / ${RaLibraryBrowser.pageCount(filtered.size)}")
                    TextButton(onClick = { page = currentPage + 1 }, enabled = currentPage + 1 < RaLibraryBrowser.pageCount(filtered.size),
                        modifier = Modifier.controllerFocusable("ra.library.next", onConfirm = {
                            if (currentPage + 1 < RaLibraryBrowser.pageCount(filtered.size)) page = currentPage + 1
                        })) { Text(str("ra.library.next")) }
                }
            }
        }
    }
}
