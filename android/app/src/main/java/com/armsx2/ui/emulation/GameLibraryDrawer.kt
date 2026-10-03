package com.armsx2.ui.emulation
import com.armsx2.ui.common.selectionOutline

import com.armsx2.ui.common.padFocusRing

import androidx.activity.compose.BackHandler
import androidx.compose.animation.AnimatedVisibility
import androidx.compose.animation.core.tween
import androidx.compose.animation.fadeIn
import androidx.compose.animation.slideInHorizontally
import androidx.compose.foundation.BorderStroke
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.itemsIndexed
import androidx.compose.foundation.lazy.rememberLazyListState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import com.armsx2.GameInfo
import com.armsx2.data.library.GameLibraryRepository
import com.armsx2.i18n.str
import com.armsx2.runtime.MainActivityRuntime
import com.armsx2.ui.WindowImpl
import com.armsx2.ui.common.GameCoverArt
import com.armsx2.ui.home.LibraryKeyboard
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext

/** Browses cached entries without mounting the launcher or probing ROMs during a session. */
@Composable
fun GameLibraryDrawer() {
    com.armsx2.data.library.GameCollections.ensureLoaded()
    val collections = com.armsx2.data.library.GameCollections.items.value
    val collectionId = com.armsx2.data.library.GameCollections.selectedId.value
    val activeCollection = collections.find { it.id == collectionId }
    var collectionsOpen by remember { mutableStateOf(false) }
    val context = LocalContext.current
    val repository = remember(context) { GameLibraryRepository(context.applicationContext) }
    var games by remember { mutableStateOf<List<GameInfo>>(emptyList()) }
    var loaded by remember { mutableStateOf(false) }
    var query by remember { mutableStateOf("") }
    var selected by remember { mutableIntStateOf(0) } // 0 = collections, 1 = search; subsequent rows = games
    var switching by remember { mutableStateOf(false) }
    var shown by remember { mutableStateOf(false) }
    val listState = rememberLazyListState()
    val searchLabel = str("games.search.placeholder")
    val current = MainActivityRuntime.currentGame.value
    val entries = remember(games, query, collections, collectionId) {
        val term = query.trim()
        games.filter { game ->
            com.armsx2.data.library.GameCollections.includes(game.uri.toString()) &&
            (com.armsx2.HiddenGames.showHidden.value || !com.armsx2.HiddenGames.isHidden(game)) &&
                (term.isBlank() || game.title.contains(term, true) ||
                    game.titleEn.contains(term, true) || game.titleSort.contains(term, true) ||
                    game.serial?.contains(term, true) == true)
        }.sortedWith(compareBy<GameInfo> {
            if (term.isBlank() || it.title.startsWith(term, true) ||
                it.titleEn.startsWith(term, true) || it.titleSort.startsWith(term, true)) 0 else 1
        }.thenBy { it.title.lowercase() })
    }
    val dismiss = { WindowImpl.dismissGameLibrary() }
    val search = {
        selected = 1
        LibraryKeyboard.open(query, { query = it; selected = 1 }, searchLabel)
    }
    val choose: (GameInfo) -> Unit = { game ->
        if (!switching) {
            if (game.uri == current?.uri) {
                dismiss()
            } else {
                switching = true
                repository.markPlayed(game)
                LibraryKeyboard.close()
                // Choosing a game is the only point that invokes the existing VM switch.
                val path = if (game.uri.scheme == "file") game.uri.path ?: game.uri.toString()
                    else game.uri.toString()
                MainActivityRuntime.launchGame(path, game)
            }
        }
    }
    LaunchedEffect(repository) {
        shown = true
        games = withContext(Dispatchers.IO) { repository.loadCached().games }
        loaded = true
    }
    LaunchedEffect(entries.size, collectionId) { selected = selected.coerceAtMost(entries.size + 1) }
    LaunchedEffect(selected) {
        if (selected > 1) listState.animateScrollToItem(selected - 2)
    }
    val token = remember { Any() }
    SideEffect {
        GameLibraryInputController.bind(token,
            move = { delta -> selected = (selected + delta).coerceIn(0, entries.size + 1) },
            confirm = { when (selected) {
                0 -> collectionsOpen = true
                1 -> search()
                else -> entries.getOrNull(selected - 2)?.let(choose)
            } },
            search = search)
    }
    DisposableEffect(token) {
        onDispose { GameLibraryInputController.unbind(token); LibraryKeyboard.close() }
    }
    BackHandler {
        if (LibraryKeyboard.visible.value) LibraryKeyboard.close() else dismiss()
    }
    BoxWithConstraints(Modifier.fillMaxSize()) {
        Box(Modifier.fillMaxSize()
            .background(MaterialTheme.colorScheme.scrim.copy(alpha = 0.42f))
            .clickable(onClick = dismiss))
        AnimatedVisibility(
            visible = shown,
            modifier = Modifier.align(Alignment.CenterEnd),
            enter = slideInHorizontally(tween(220)) { it } + fadeIn(tween(160)),
        ) {
            // Leave part of the paused game visible in both portrait and landscape.
            Surface(onClick = {}, modifier = Modifier.width((maxWidth * 0.72f).coerceAtMost(460.dp))
                .fillMaxHeight(), shape = RoundedCornerShape(topStart = 22.dp, bottomStart = 22.dp),
                tonalElevation = 6.dp, shadowElevation = 12.dp) {
                Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(12.dp)) {
                    Row(verticalAlignment = Alignment.CenterVertically) {
                        Text(str("action.swapGame"), Modifier.weight(1f),
                            style = MaterialTheme.typography.titleLarge, fontWeight = FontWeight.Bold)
                        TextButton(modifier = Modifier.padFocusRing(), onClick = dismiss) { Text(str("action.resume")) }
                    }
                    Surface(onClick = { collectionsOpen = true },
                        modifier = Modifier.fillMaxWidth().selectionOutline(
                            selected == 0 && !collectionsOpen && !LibraryKeyboard.visible.value, RoundedCornerShape(12.dp)),
                        shape = RoundedCornerShape(12.dp),
                        border = BorderStroke(1.dp, MaterialTheme.colorScheme.outline)) {
                        Column(Modifier.padding(12.dp)) {
                            Text(str("collections.title"), style = MaterialTheme.typography.labelMedium)
                            Text(activeCollection?.name ?: str("collections.all"), maxLines = 1, overflow = TextOverflow.Ellipsis)
                        }
                    }
                    Surface(onClick = search, modifier = Modifier.fillMaxWidth()
                        .selectionOutline(selected == 1 && !collectionsOpen && !LibraryKeyboard.visible.value, RoundedCornerShape(12.dp)),
                        shape = RoundedCornerShape(12.dp),
                        border = BorderStroke(1.dp, MaterialTheme.colorScheme.outline)) {
                        Text(query.ifBlank { searchLabel }, Modifier.padding(14.dp),
                            maxLines = 1, overflow = TextOverflow.Ellipsis)
                    }
                    when {
                        !loaded -> Text(str("action.swapGame.loading"))
                        entries.isEmpty() -> Text(str("action.swapGame.empty"))
                    }
                    LazyColumn(state = listState, modifier = Modifier.weight(1f),
                        verticalArrangement = Arrangement.spacedBy(8.dp)) {
                        itemsIndexed(entries, key = { _, game -> game.uri.toString() }) { index, game ->
                            val active = selected == index + 2 && !collectionsOpen && !LibraryKeyboard.visible.value
                            Surface(onClick = { selected = index + 2; choose(game) },
                                modifier = Modifier.fillMaxWidth().selectionOutline(active, RoundedCornerShape(14.dp)), shape = RoundedCornerShape(14.dp),
                                color = if (active) MaterialTheme.colorScheme.primaryContainer else MaterialTheme.colorScheme.surfaceVariant,
                                border = null) {
                                Row(Modifier.padding(10.dp), verticalAlignment = Alignment.CenterVertically,
                                    horizontalArrangement = Arrangement.spacedBy(12.dp)) {
                                    GameCoverArt(game, Modifier.size(66.dp))
                                    Column(Modifier.weight(1f)) {
                                        Text(game.title, maxLines = 2, overflow = TextOverflow.Ellipsis,
                                            fontWeight = FontWeight.SemiBold)
                                        if (game.uri == current?.uri) Text(str("action.swapGame.current"),
                                            style = MaterialTheme.typography.labelMedium)
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }
    if (collectionsOpen) {
        com.armsx2.ui.home.CollectionsOverlay(games) { collectionsOpen = false }
    }
}

/** Dedicated list navigation also reaches entries that LazyColumn has not composed yet. */
object GameLibraryInputController {
    private var owner: Any? = null
    private var moveAction: (Int) -> Unit = {}
    private var confirmAction: () -> Unit = {}
    private var searchAction: () -> Unit = {}
    fun active() = WindowImpl.showLibrary.value && owner != null
    fun bind(token: Any, move: (Int) -> Unit, confirm: () -> Unit, search: () -> Unit) {
        owner = token; moveAction = move; confirmAction = confirm; searchAction = search
    }
    fun unbind(token: Any) {
        if (owner === token) { owner = null; moveAction = {}; confirmAction = {}; searchAction = {} }
    }
    fun move(delta: Int) { if (active()) moveAction(delta) }
    fun confirm() { if (active()) confirmAction() }
    fun search() { if (active()) searchAction() }
}
