package com.armsx2.ui.home

import android.view.KeyEvent
import android.widget.Toast
import androidx.activity.compose.BackHandler
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.itemsIndexed
import androidx.compose.foundation.lazy.rememberLazyListState
import androidx.compose.foundation.lazy.grid.GridCells
import androidx.compose.foundation.lazy.grid.LazyVerticalGrid
import androidx.compose.foundation.lazy.grid.itemsIndexed
import androidx.compose.foundation.lazy.grid.rememberLazyGridState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.*
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import com.armsx2.EnglishTitles
import com.armsx2.GameInfo
import com.armsx2.data.library.CollectionPolicy
import com.armsx2.data.library.GameCollections
import com.armsx2.data.library.FolderArtwork
import com.armsx2.i18n.str
import com.armsx2.ui.common.selectionOutline
import com.armsx2.ui.common.ArmsTopBar
import com.armsx2.ui.common.RoundAction
import com.armsx2.ui.settings.SettingsControllerNav

private enum class CollectionPage { Browse, Games, Membership, Name, Delete, HomeTile }
private data class CollectionRow(
    val key: String, val label: String, val detail: String? = null,
    val checked: Boolean? = null, val edit: (() -> Unit)? = null, val enabled: Boolean = true,
    val action: () -> Unit,
)

/** Shared collection editor: a library page on Home, an overlay in the in-session drawer. */
@Composable
fun CollectionsOverlay(games: List<GameInfo>, initialGame: GameInfo? = null,
    embedded: Boolean = false, initialEditingId: String? = null, initialCreate: Boolean = false,
    onDismiss: () -> Unit) {
    GameCollections.ensureLoaded()
    val collections = GameCollections.items.value
    var page by remember { mutableStateOf(when {
        initialGame != null -> CollectionPage.Membership
        initialCreate -> CollectionPage.Name
        initialEditingId == CollectionPolicy.HomeFolderKey -> CollectionPage.HomeTile
        initialEditingId != null -> CollectionPage.Games
        else -> CollectionPage.Browse
    }) }
    var editingId by remember { mutableStateOf(initialEditingId) }
    var name by remember { mutableStateOf("") }
    var query by remember { mutableStateOf("") }
    var error by remember { mutableStateOf(false) }
    var selected by remember { mutableIntStateOf(0) }
    val listState = rememberLazyListState()
    val gridState = rememberLazyGridState()
    var gridColumns by remember { mutableIntStateOf(1) }
    val folderGrid = embedded && page == CollectionPage.Browse && !GameCollections.readFailed.value
    val collection = collections.find { it.id == editingId }
    val context = LocalContext.current
    var artworkTarget by rememberSaveable { mutableStateOf<String?>(null) }
    val artworkError = str("collections.imageError")
    val artworkPicker = rememberLauncherForActivityResult(ActivityResultContracts.OpenDocument()) { uri ->
        val target = artworkTarget
        artworkTarget = null
        if (uri != null && target != null) runCatching { FolderArtwork.set(context, target, uri) }
            .onFailure { Toast.makeText(context, artworkError, Toast.LENGTH_LONG).show() }
    }
    val artworkVersion = FolderArtwork.version.intValue
    val currentArtwork = remember(editingId, artworkVersion) { editingId?.let(FolderArtwork::image) }
    val nameLabel = str("collections.name")
    val searchLabel = str("games.search.placeholder")
    val allLabel = str("collections.all")
    val newLabel = str("collections.create")
    val editLabel = str("collections.edit")
    val renameLabel = str("collections.rename")
    val deleteLabel = str("collections.delete")
    val saveLabel = str("action.save")
    val cancelLabel = str("action.cancel")
    val doneLabel = str("collections.done")
    val gamesLabel = str("collections.games")
    val chooseImageLabel = str("collections.chooseImage")
    val resetImageLabel = str("collections.resetImage")
    val moveLeftLabel = str("collections.moveLeft")
    val moveRightLabel = str("collections.moveRight")
    val returnPage = if (initialGame == null) CollectionPage.Browse else CollectionPage.Membership

    fun show(next: CollectionPage) { page = next; selected = 0; error = false }
    fun edit(id: String) { editingId = id; query = ""; show(CollectionPage.Games) }
    fun create() { editingId = null; name = ""; show(CollectionPage.Name) }
    fun leaveEditor() {
        if (embedded && (initialEditingId != null || initialCreate)) onDismiss() else show(returnPage)
    }
    val back: () -> Unit = {
        when (page) {
            CollectionPage.Browse, CollectionPage.Membership -> onDismiss()
            CollectionPage.Name -> if (editingId == null) leaveEditor() else show(CollectionPage.Games)
            CollectionPage.Delete -> show(CollectionPage.Games)
            CollectionPage.Games, CollectionPage.HomeTile -> leaveEditor()
        }
    }
    val search: () -> Unit = {
        if (page == CollectionPage.Games) {
            selected = 0
            LibraryKeyboard.open(query, { query = it; selected = 0 }, searchLabel)
        } else if (page == CollectionPage.Name) {
            LibraryKeyboard.open(name, { name = it; error = false }, nameLabel)
        }
    }
    val rows = buildList {
        fun folderOptions(id: String, movable: Boolean) {
            add(CollectionRow("choose-image", chooseImageLabel) {
                artworkTarget = id
                artworkPicker.launch(arrayOf("image/*"))
            })
            if (currentArtwork != null) add(CollectionRow("reset-image", resetImageLabel) {
                FolderArtwork.clear(id)
            })
            if (movable) {
                add(CollectionRow("move-left", moveLeftLabel, enabled = GameCollections.canMoveHomeTile(id, -1)) {
                    GameCollections.moveHomeTile(id, -1)
                })
                add(CollectionRow("move-right", moveRightLabel, enabled = GameCollections.canMoveHomeTile(id, 1)) {
                    GameCollections.moveHomeTile(id, 1)
                })
            }
        }
        if (GameCollections.readFailed.value) {
            add(CollectionRow("close", doneLabel, action = onDismiss))
            return@buildList
        }
        when (page) {
            CollectionPage.Browse -> {
                add(CollectionRow("all", allLabel, checked = GameCollections.selectedId.value == null) {
                    GameCollections.select(null); onDismiss()
                })
                add(CollectionRow("new", newLabel, action = ::create))
                collections.forEach { item ->
                    val count = games.count { it.uri.toString() in item.gameUris }
                    add(CollectionRow(item.id, item.name, "$count $gamesLabel",
                        GameCollections.selectedId.value == item.id, edit = { edit(item.id) }) {
                        GameCollections.select(item.id); onDismiss()
                    })
                }
            }
            CollectionPage.Membership -> {
                add(CollectionRow("done", doneLabel, action = onDismiss))
                add(CollectionRow("new", newLabel, action = ::create))
                collections.forEach { item ->
                    val uri = initialGame!!.uri.toString()
                    val member = uri in item.gameUris
                    add(CollectionRow(item.id, item.name, checked = member) {
                        GameCollections.setMember(item.id, uri, !member)
                    })
                }
            }
            CollectionPage.Games -> if (collection != null) {
                add(CollectionRow("search", query.ifBlank { searchLabel }, action = search))
                add(CollectionRow("done", doneLabel, action = ::leaveEditor))
                add(CollectionRow("home", str(if (collection.onHome) "collections.removeHome" else "collections.addHome"),
                    checked = collection.onHome) { GameCollections.setOnHome(collection.id, !collection.onHome) })
                folderOptions(collection.id, collection.onHome)
                add(CollectionRow("rename", renameLabel) { name = collection.name; show(CollectionPage.Name) })
                add(CollectionRow("delete", deleteLabel) { show(CollectionPage.Delete) })
                games.filter { query.isBlank() || it.displayTitle(EnglishTitles.enabled.value).contains(query.trim(), true) ||
                    it.title.contains(query.trim(), true) || it.titleEn.contains(query.trim(), true) ||
                    it.serial?.contains(query.trim(), true) == true }
                    .sortedBy { it.sortKey(EnglishTitles.enabled.value).lowercase() }.forEach { game ->
                        val uri = game.uri.toString()
                        val member = uri in collection.gameUris
                        add(CollectionRow(uri, game.displayTitle(EnglishTitles.enabled.value), game.serial,
                            checked = member) { GameCollections.setMember(collection.id, uri, !member) })
                    }
            }
            CollectionPage.Name -> {
                add(CollectionRow("name", name.ifBlank { nameLabel }, action = search))
                add(CollectionRow("save", saveLabel) {
                    if (!CollectionPolicy.validName(collections, name, editingId)) error = true
                    else if (editingId == null) {
                        val created = GameCollections.create(name, initialGame?.uri?.toString())
                        if (initialGame != null) show(returnPage) else edit(created.id)
                    } else {
                        GameCollections.rename(editingId!!, name); show(CollectionPage.Games)
                    }
                })
                add(CollectionRow("cancel", cancelLabel, action = back))
            }
            CollectionPage.Delete -> {
                add(CollectionRow("cancel", cancelLabel, action = back))
                add(CollectionRow("delete", deleteLabel) {
                    editingId?.let(GameCollections::delete); editingId = null; leaveEditor()
                })
            }
            CollectionPage.HomeTile -> {
                add(CollectionRow("done", doneLabel, action = ::leaveEditor))
                folderOptions(CollectionPolicy.HomeFolderKey, true)
            }
        }
    }
    val token = remember { Any() }
    SideEffect {
        CollectionsInputController.bind(token,
            move = { selected = (selected + it * if (folderGrid) gridColumns else 1)
                .coerceIn(0, (rows.size - 1).coerceAtLeast(0)) },
            confirm = { rows.getOrNull(selected)?.takeIf { it.enabled }?.action?.invoke() },
            edit = { rows.getOrNull(selected)?.edit?.invoke() }, back = back, search = search,
            horizontal = { delta ->
                val next = selected + delta
                if (folderGrid && next in rows.indices && next / gridColumns == selected / gridColumns) selected = next
            })
    }
    DisposableEffect(token) {
        val releaseLayer = SettingsControllerNav.claimLayer("library.collections")
        onDispose { CollectionsInputController.unbind(token); LibraryKeyboard.close(); releaseLayer() }
    }
    LaunchedEffect(selected, page, rows.size) {
        selected = selected.coerceIn(0, (rows.size - 1).coerceAtLeast(0))
        if (rows.isNotEmpty()) {
            if (folderGrid) gridState.animateScrollToItem(selected) else listState.animateScrollToItem(selected)
        }
    }
    BackHandler { if (LibraryKeyboard.visible.value) LibraryKeyboard.close() else back() }
    val title = when (page) {
        CollectionPage.Name -> if (editingId == null) newLabel else renameLabel
        CollectionPage.Games, CollectionPage.Delete -> collection?.name ?: str("collections.title")
        else -> str("collections.title")
    }
    val content: @Composable () -> Unit = {
            Column(Modifier.fillMaxSize().padding(if (embedded) 8.dp else 16.dp),
                verticalArrangement = Arrangement.spacedBy(10.dp)) {
                if (embedded) {
                    ArmsTopBar(title, leading = {
                        RoundAction("←", str("collections.back"), back, framed = true,
                            buttonSize = 44.dp, buttonShape = RoundedCornerShape(14.dp), subtleFrame = true)
                    }, horizontalPadding = 0.dp)
                } else Row(verticalAlignment = Alignment.CenterVertically) {
                    Text(title, Modifier.weight(1f), style = MaterialTheme.typography.titleLarge, fontWeight = FontWeight.Bold)
                    TextButton(onClick = back) { Text(str("collections.back")) }
                }
                if (initialGame != null && page == CollectionPage.Membership)
                    Text(initialGame.displayTitle(EnglishTitles.enabled.value), maxLines = 2, overflow = TextOverflow.Ellipsis)
                Surface(color = MaterialTheme.colorScheme.surface.copy(alpha = 0.92f),
                    shape = RoundedCornerShape(10.dp)) {
                    Text(str(when (page) {
                    CollectionPage.Games -> "collections.membershipHint"
                    CollectionPage.Membership -> "collections.gameHint"
                    CollectionPage.Delete -> "collections.deleteHint"
                    CollectionPage.Name -> "collections.nameHint"
                    CollectionPage.Browse -> "collections.browseHint"
                    CollectionPage.HomeTile -> "collections.moveHint"
                    }), modifier = Modifier.padding(horizontal = 12.dp, vertical = 8.dp),
                        style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
                }
                if (error) Text(str("collections.invalidName"), color = MaterialTheme.colorScheme.error)
                if (GameCollections.readFailed.value)
                    Text(str("collections.readError"), color = MaterialTheme.colorScheme.error)
                if (folderGrid) BoxWithConstraints(Modifier.weight(1f).fillMaxWidth()) {
                    val columns = (maxWidth.value / 126f).toInt().coerceAtLeast(1)
                    SideEffect { gridColumns = columns }
                    LazyVerticalGrid(columns = GridCells.Fixed(columns), state = gridState,
                        modifier = Modifier.fillMaxSize(), contentPadding = PaddingValues(bottom = 16.dp),
                        horizontalArrangement = Arrangement.spacedBy(12.dp),
                        verticalArrangement = Arrangement.spacedBy(16.dp)) {
                        itemsIndexed(rows, key = { _, row -> row.key }) { index, row ->
                            Column {
                                CollectionHomeTile(
                                    selected = selected == index && !LibraryKeyboard.visible.value,
                                    label = row.label, onEdit = row.edit,
                                    artworkKey = row.key,
                                    glyph = if (row.key == "new") "+" else "▤") { selected = index; row.action() }
                                row.edit?.let { action ->
                                    Surface(color = MaterialTheme.colorScheme.surface, shape = RoundedCornerShape(12.dp)) {
                                        TextButton(onClick = action) { Text(editLabel) }
                                    }
                                }
                            }
                        }
                    }
                } else LazyColumn(state = listState, modifier = Modifier.weight(1f),
                    verticalArrangement = Arrangement.spacedBy(8.dp)) {
                    itemsIndexed(rows, key = { _, row -> row.key }) { index, row ->
                        Surface(onClick = { selected = index; row.action() }, enabled = row.enabled,
                            modifier = Modifier.fillMaxWidth().selectionOutline(
                                selected == index && !LibraryKeyboard.visible.value, RoundedCornerShape(12.dp)),
                            shape = RoundedCornerShape(12.dp), color = MaterialTheme.colorScheme.surfaceVariant) {
                            Row(Modifier.padding(12.dp), verticalAlignment = Alignment.CenterVertically,
                                horizontalArrangement = Arrangement.spacedBy(10.dp)) {
                                row.checked?.let { Text(if (it) "☑" else "☐", color = MaterialTheme.colorScheme.primary) }
                                Column(Modifier.weight(1f)) {
                                    Text(row.label, maxLines = 2, overflow = TextOverflow.Ellipsis)
                                    row.detail?.let { Text(it, style = MaterialTheme.typography.bodySmall) }
                                }
                                row.edit?.let { action -> TextButton(onClick = action) { Text(editLabel) } }
                            }
                        }
                    }
                }
            }
    }
    if (embedded) content() else BoxWithConstraints(Modifier.fillMaxSize()) {
        Box(Modifier.fillMaxSize().background(MaterialTheme.colorScheme.scrim.copy(alpha = 0.6f))
            .clickable(onClick = onDismiss))
        Surface(onClick = {}, modifier = Modifier.align(Alignment.Center).width((maxWidth * 0.92f).coerceAtMost(620.dp))
            .height(maxHeight * 0.90f), shape = RoundedCornerShape(22.dp), tonalElevation = 8.dp) { content() }
    }
}

/** Captures A until release, so opening a child page cannot also press its first action. */
object CollectionsInputController {
    private var owner: Any? = null
    private var moveAction: (Int) -> Unit = {}
    private var confirmAction: () -> Unit = {}
    private var editAction: () -> Unit = {}
    private var backAction: () -> Unit = {}
    private var searchAction: () -> Unit = {}
    private var horizontalAction: (Int) -> Unit = {}
    private var pendingConfirm: (() -> Unit)? = null
    fun active() = owner != null
    fun bind(token: Any, move: (Int) -> Unit, confirm: () -> Unit, edit: () -> Unit,
             back: () -> Unit, search: () -> Unit, horizontal: (Int) -> Unit = {}) {
        owner = token; moveAction = move; confirmAction = confirm
        editAction = edit; backAction = back; searchAction = search
        horizontalAction = horizontal
    }
    fun unbind(token: Any) {
        if (owner === token) {
            owner = null; pendingConfirm = null; moveAction = {}; confirmAction = {}
            editAction = {}; backAction = {}; searchAction = {}; horizontalAction = {}
        }
    }
    fun move(delta: Int) { pendingConfirm = null; if (active()) moveAction(delta) }
    fun moveHorizontal(delta: Int) { pendingConfirm = null; if (active()) horizontalAction(delta) }
    fun key(event: KeyEvent): Boolean {
        if (!active()) return false
        val down = event.action == KeyEvent.ACTION_DOWN
        when (event.keyCode) {
            KeyEvent.KEYCODE_BUTTON_A, KeyEvent.KEYCODE_DPAD_CENTER,
            KeyEvent.KEYCODE_ENTER, KeyEvent.KEYCODE_NUMPAD_ENTER -> {
                if (down && event.repeatCount == 0) pendingConfirm = confirmAction
                else if (event.action == KeyEvent.ACTION_UP) {
                    val action = pendingConfirm; pendingConfirm = null; action?.invoke()
                }
            }
            KeyEvent.KEYCODE_DPAD_UP -> if (down) move(-1)
            KeyEvent.KEYCODE_DPAD_DOWN -> if (down) move(1)
            KeyEvent.KEYCODE_DPAD_LEFT -> if (down) moveHorizontal(-1)
            KeyEvent.KEYCODE_DPAD_RIGHT -> if (down) moveHorizontal(1)
            KeyEvent.KEYCODE_BUTTON_X -> if (down && event.repeatCount == 0) { pendingConfirm = null; editAction() }
            KeyEvent.KEYCODE_BUTTON_Y -> if (down && event.repeatCount == 0) { pendingConfirm = null; searchAction() }
            KeyEvent.KEYCODE_BUTTON_B, KeyEvent.KEYCODE_BACK, KeyEvent.KEYCODE_BUTTON_START ->
                if (down && event.repeatCount == 0) { pendingConfirm = null; backAction() }
        }
        return true
    }
}
