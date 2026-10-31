package com.armsx2.ui.achievements

import androidx.activity.compose.BackHandler
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.relocation.BringIntoViewRequester
import androidx.compose.foundation.relocation.bringIntoViewRequester
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.produceState
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberUpdatedState
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.runtime.withFrameNanos
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import com.armsx2.CustomCovers
import com.armsx2.GameInfo
import com.armsx2.RaLibrary
import com.armsx2.achievements.RaGameDetails
import com.armsx2.achievements.RaDetailsNavigation
import com.armsx2.achievements.RaLibraryBrowser
import com.armsx2.i18n.str
import com.armsx2.ui.common.EmptyState
import com.armsx2.ui.common.FallbackCoverImage
import com.armsx2.ui.common.GameCoverPlaceholder
import com.armsx2.ui.settings.controllerFocusable
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext

private data class DetailState(val path: String, val account: String, val loaded: RaGameDetails.Loaded? = null, val error: String? = null)

@Composable
internal fun LibraryGameAchievements(game: GameInfo, title: String, account: String, onBack: () -> Unit) {
    val context = LocalContext.current.applicationContext
    val path = game.uri.toString()
    var refresh by remember(path, account) { mutableIntStateOf(0) }
    val state by produceState(DetailState(path, account), path, account, refresh) {
        value = DetailState(path, account)
        value = withContext(Dispatchers.IO) {
            runCatching { RaLibrary.loadGameDetails(context, game, account, refresh > 0) }
                .fold({ DetailState(path, account, loaded = it) }, {
                    DetailState(path, account, error = it.message ?: "Could not load achievements. Please try again.")
                })
        }
    }
    val current = state.takeIf { it.path == path && it.account == account }
    val loaded = current?.loaded
    var query by rememberSaveable(path, account) { mutableStateOf("") }
    var filter by rememberSaveable(path, account) { mutableStateOf(RaGameDetails.Filter.ALL) }
    var page by rememberSaveable(path, account, query, filter) { mutableIntStateOf(0) }
    val all = loaded?.game?.items.orEmpty()
    val items = remember(all, query, filter) { RaGameDetails.select(all, query, filter) }
    val currentPage = RaLibraryBrowser.safePage(page, items.size)
    val anchor = remember { BringIntoViewRequester() }
    val currentBack by rememberUpdatedState(onBack)
    DisposableEffect(path, account) {
        val release = RaDetailsNavigation.open { currentBack() }
        onDispose { release() }
    }
    BackHandler(onBack = onBack)
    LaunchedEffect(path, currentPage, query, filter) { withFrameNanos { }; anchor.bringIntoView() }
    val customCover = remember(game.uri, CustomCovers.version.value) { CustomCovers.fileFor(context, game) }
    val artwork = listOfNotNull(customCover, loaded?.game?.artUrl) + game.coverUrls

    Column(Modifier.fillMaxWidth(), verticalArrangement = Arrangement.spacedBy(10.dp)) {
        Row(Modifier.fillMaxWidth().bringIntoViewRequester(anchor), horizontalArrangement = Arrangement.SpaceBetween) {
            TextButton(onClick = onBack, modifier = Modifier.controllerFocusable("ra.details.back", onConfirm = onBack)) {
                Text(str("ra.details.back"))
            }
            TextButton(onClick = { refresh++ }, enabled = loaded != null || current?.error != null,
                modifier = Modifier.controllerFocusable("ra.details.refresh", onConfirm = {
                    if (loaded != null || current?.error != null) refresh++
                })) { Text(str("games.card.refresh")) }
        }
        Row(horizontalArrangement = Arrangement.spacedBy(12.dp), verticalAlignment = Alignment.CenterVertically) {
            FallbackCoverImage(artwork, title, Modifier.size(84.dp), ContentScale.Fit) { GameCoverPlaceholder(title) }
            Column(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(4.dp)) {
                Text(loaded?.game?.title?.takeIf(String::isNotBlank) ?: title, style = MaterialTheme.typography.titleLarge)
                if (loaded != null) Text("${all.count { it.softcore }} / ${all.size} SC · ${all.count { it.hardcore }} / ${all.size} HC")
            }
        }
        when {
            current?.error != null -> EmptyState(str("ra.details.error"), current.error)
            loaded == null -> {
                CircularProgressIndicator(Modifier.size(24.dp))
                Text(str("ra.details.loading"), style = MaterialTheme.typography.bodySmall)
            }
            else -> {
                if (loaded.stale) Text(str("ra.details.saved"), style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant)
                OutlinedTextField(value = query, onValueChange = { query = it }, singleLine = true,
                    label = { Text(str("ra.details.search")) }, modifier = Modifier.fillMaxWidth())
                Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                    RaGameDetails.Filter.entries.forEach { mode ->
                        TextButton(onClick = { filter = mode }, modifier = Modifier.controllerFocusable("ra.details.filter.${mode.name}", onConfirm = { filter = mode })) {
                            Text(str(when (mode) {
                                RaGameDetails.Filter.ALL -> "ra.filter.all"
                                RaGameDetails.Filter.UNLOCKED -> "ra.filter.unlocked"
                                RaGameDetails.Filter.LOCKED -> "ra.filter.locked"
                            }), fontWeight = if (mode == filter) FontWeight.Bold else FontWeight.Normal)
                        }
                    }
                }
                if (items.isEmpty()) EmptyState(str("ra.details.empty"), str("ra.details.empty.desc"))
                RaLibraryBrowser.page(items, currentPage).forEach { item ->
                    val unlocked = item.softcore
                    val label = str(if (item.hardcore) "ra.details.hardcore" else if (unlocked) "ra.details.unlocked" else "ra.details.locked")
                    Box(Modifier.fillMaxWidth().controllerFocusable("ra.details.achievement.${item.id}")) {
                        AchievementRow(AchievementItem(item.id, item.title, item.description, item.points,
                            unlocked, if (item.hardcore) 3 else if (unlocked) 1 else 0,
                            label, item.badgeUrl.orEmpty(), type = item.type), expanded = true)
                    }
                }
                if (items.size > RaLibraryBrowser.PAGE_SIZE) Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.SpaceBetween,
                    verticalAlignment = Alignment.CenterVertically) {
                    TextButton(onClick = { page = currentPage - 1 }, enabled = currentPage > 0,
                        modifier = Modifier.controllerFocusable("ra.details.previous", onConfirm = { if (currentPage > 0) page = currentPage - 1 })) {
                        Text(str("ra.library.previous"))
                    }
                    Text("${currentPage + 1} / ${RaLibraryBrowser.pageCount(items.size)}")
                    TextButton(onClick = { page = currentPage + 1 }, enabled = currentPage + 1 < RaLibraryBrowser.pageCount(items.size),
                        modifier = Modifier.controllerFocusable("ra.details.next", onConfirm = {
                            if (currentPage + 1 < RaLibraryBrowser.pageCount(items.size)) page = currentPage + 1
                        })) { Text(str("ra.library.next")) }
                }
            }
        }
    }
}
