package com.armsx2.ui.home
import com.armsx2.ui.common.selectionOutline
import com.armsx2.ui.common.SelectionBlue

import com.armsx2.ui.common.padFocusRing

import android.widget.Toast
import androidx.activity.compose.BackHandler
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.BorderStroke
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.clickable
import androidx.compose.foundation.ExperimentalFoundationApi
import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.combinedClickable
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.foundation.gestures.scrollBy
import androidx.compose.foundation.gestures.detectDragGesturesAfterLongPress
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.WindowInsets
import androidx.compose.foundation.layout.asPaddingValues
import androidx.compose.foundation.layout.calculateEndPadding
import androidx.compose.foundation.layout.calculateStartPadding
import androidx.compose.foundation.layout.displayCutout
import androidx.compose.foundation.layout.navigationBars
import androidx.compose.foundation.layout.statusBars
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.aspectRatio
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.lazy.LazyRow
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.lazy.itemsIndexed
import androidx.compose.foundation.lazy.rememberLazyListState
import androidx.compose.foundation.lazy.grid.GridCells
import androidx.compose.foundation.lazy.grid.GridItemSpan
import androidx.compose.foundation.lazy.grid.LazyGridScope
import androidx.compose.foundation.lazy.grid.items
import androidx.compose.foundation.lazy.grid.LazyVerticalGrid
import androidx.compose.foundation.lazy.grid.itemsIndexed
import androidx.compose.foundation.lazy.grid.rememberLazyGridState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.DropdownMenu
import androidx.compose.material3.DropdownMenuItem
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.ModalBottomSheet
import androidx.compose.material3.Surface
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.CompositionLocalProvider
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.SideEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.mutableFloatStateOf
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberUpdatedState
import androidx.compose.runtime.setValue
import androidx.compose.runtime.staticCompositionLocalOf
import androidx.compose.runtime.withFrameNanos
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.draw.clipToBounds
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.Path
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.graphics.graphicsLayer
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.res.painterResource
import com.armsx2.CoverArtStyle
import com.armsx2.EnglishTitles
import com.armsx2.GridLabels
import com.armsx2.R
import com.armsx2.ui.theme.ToolbarPositionPreferences
import com.armsx2.ui.theme.LibraryChromePreferences
import androidx.compose.ui.layout.layout
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.viewinterop.AndroidView
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.focus.FocusRequester
import androidx.compose.ui.platform.LocalConfiguration
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalLayoutDirection
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.platform.LocalSoftwareKeyboardController
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.lifecycle.viewmodel.compose.viewModel
import coil.compose.AsyncImage
import coil.request.ImageRequest
import com.armsx2.ui.common.FallbackCoverImage
import com.armsx2.CustomCovers
import com.armsx2.GameInfo
import com.armsx2.i18n.str
import com.armsx2.runtime.MainActivityRuntime
import com.armsx2.ui.common.ArmsBackdrop
import com.armsx2.ui.common.ArmsTopBar
import com.armsx2.ui.common.EmptyState
import com.armsx2.ui.common.RoundAction
import com.armsx2.ui.common.SearchField
import com.armsx2.ui.common.SectionTitle
import com.armsx2.ui.common.StatusChip
import kotlin.math.abs

private val LocalCustomCoverMap = staticCompositionLocalOf<Map<String, java.io.File>> { emptyMap() }
private data class HomeFolderEntry(val key: String, val label: String,
    val game: GameInfo? = null, val expanded: Boolean? = null, val glyph: String = "▤",
    val edit: (() -> Unit)? = null, val action: () -> Unit)

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun HomeScreen(
    onOpenMenu: () -> Unit,
    onOpenGameSettings: (GameInfo) -> Unit,
    modifier: Modifier = Modifier,
    viewModel: HomeViewModel = viewModel(),
) {
    com.armsx2.data.library.GameCollections.ensureLoaded()
    val collections = com.armsx2.data.library.GameCollections.items.value
    val collectionId = com.armsx2.data.library.GameCollections.selectedId.value
    val activeCollection = collections.find { it.id == collectionId }
    var foldersExpanded by remember { mutableStateOf(false) }
    var expandedCollectionId by remember { mutableStateOf<String?>(null) }
    var collectionsOpen by remember { mutableStateOf(false) }
    var collectionGame by remember { mutableStateOf<GameInfo?>(null) }
    var editingCollectionId by remember { mutableStateOf<String?>(null) }
    var creatingCollection by remember { mutableStateOf(false) }
    fun openCollections(game: GameInfo? = null, editId: String? = null, create: Boolean = false) {
        collectionGame = game
        editingCollectionId = editId
        creatingCollection = create
        collectionsOpen = true
    }
    fun toggleFolders() {
        com.armsx2.data.library.GameCollections.select(null)
        foldersExpanded = !foldersExpanded
        expandedCollectionId = null
        HomeInputController.collectionIndex.intValue = 0
    }
    fun toggleCollection(id: String) {
        com.armsx2.data.library.GameCollections.select(null)
        expandedCollectionId = if (expandedCollectionId == id) null else id
    }
    LaunchedEffect(collections, collectionId) { viewModel.collectionsChanged() }
    val state = viewModel.state.value
    val directories = MainActivityRuntime.romsDirs.value
    val nativeReady = MainActivityRuntime.nativeReady.value
    val context = LocalContext.current
    val coverVersion = CustomCovers.version.value
    val customCoverMap = remember(coverVersion) { CustomCovers.loadAll(context) }
    var overflowMenu by remember { mutableStateOf(false) }
    var showExitConfirm by remember { mutableStateOf(false) }
    var menuGame by remember { mutableStateOf<GameInfo?>(null) }
    var showClearRecentsConfirm by remember { mutableStateOf(false) }
    val editFolderLabel = str("collections.edit")
    val folderEntries = buildList {
        fun addFolder(item: com.armsx2.data.library.GameCollection) {
            val members = state.allGames.filter { it.uri.toString() in item.gameUris }
            add(HomeFolderEntry(item.id, item.name, expanded = expandedCollectionId == item.id,
                edit = { openCollections(editId = item.id) }) { toggleCollection(item.id) })
            if (expandedCollectionId == item.id) {
                if (members.isEmpty()) add(HomeFolderEntry("${item.id}/add-games", editFolderLabel, glyph = "+") {
                    openCollections(editId = item.id)
                })
                members.forEach { game ->
                    add(HomeFolderEntry("${item.id}/${game.uri}", game.displayTitle(EnglishTitles.enabled.value),
                        game = game, edit = { menuGame = game }) { viewModel.launch(game) })
                }
            }
        }
        com.armsx2.data.library.GameCollections.homeTileKeys().forEach { key ->
            if (key == com.armsx2.data.library.CollectionPolicy.HomeFolderKey) {
                add(HomeFolderEntry(key, str("collections.title"), expanded = foldersExpanded,
                    edit = { openCollections(editId = key) }, action = ::toggleFolders))
                if (foldersExpanded) {
                    add(HomeFolderEntry("new-collection", str("collections.create"), glyph = "+") {
                        openCollections(create = true)
                    })
                    collections.filterNot { it.onHome }.forEach(::addFolder)
                }
            } else collections.find { it.id == key }?.let(::addFolder)
        }
    }
    SideEffect {
        HomeInputController.setCollectionActions(
            if (state.initialized) folderEntries.map { it.action } else emptyList(),
            folderEntries.map { it.edit }, keys = folderEntries.map { it.key }, collapse = {
                when {
                    expandedCollectionId != null -> {
                        HomeInputController.collectionIndex.intValue = folderEntries.indexOfFirst {
                            it.key == expandedCollectionId
                        }.coerceAtLeast(0)
                        expandedCollectionId = null
                        true
                    }
                    foldersExpanded -> { foldersExpanded = false; HomeInputController.collectionIndex.intValue = 0; true }
                    else -> false
                }
            })
    }
    // Custom library navigation yields its outline to drawers, prompts and the keyboard.
    val libraryFocus = !collectionsOpen && !overflowMenu && menuGame == null && !showClearRecentsConfirm &&
        !LibraryKeyboard.visible.value &&
        com.armsx2.ui.settings.SettingsControllerNav.activeLayer.value == null &&
        !com.armsx2.ui.settings.SettingsControllerNav.hasSelection()
    val gridFocus = libraryFocus && HomeInputController.zone.value == HomeZone.Grid
    val drawerOpen = com.armsx2.navigation.UiNavigator.drawerOpen.value
    LaunchedEffect(libraryFocus, drawerOpen) {
        if (!libraryFocus || drawerOpen) HomeInputController.finishTileMove()
    }
    // A collection is a library sub-view. Dialogs and the drawer keep their own Back.
    BackHandler(enabled = (activeCollection != null || foldersExpanded || expandedCollectionId != null ||
        HomeInputController.movingTile.value != null) && libraryFocus &&
        !com.armsx2.navigation.UiNavigator.drawerOpen.value) {
        HomeInputController.back()
    }
    // #9 custom library background — inert until the user picks an image.
    LaunchedEffect(Unit) { LibraryBackground.ensureLoaded(); CoverArtStyle.load() }
    val backgroundPicker = rememberLauncherForActivityResult(ActivityResultContracts.OpenDocument()) { picked ->
        picked?.let { LibraryBackground.set(context, it) }
    }
    // Search: both the controller (A on the Search zone) AND a touch tap open the app's own D-pad +
    // touch keyboard (LibraryKeyboard). The search bar is no longer an editable TextField, so the
    // Android system IME never appears. (Registered once the library has loaded.)
    val showSearch = LibraryChromePreferences.showSearch.value
    val showRecents = LibraryChromePreferences.showRecents.value
    val searchPlaceholder = str("games.search.placeholder")
    LaunchedEffect(state.initialized, showSearch) {
        if (!showSearch && viewModel.state.value.query.isNotEmpty()) viewModel.setQuery("")
        HomeInputController.setSearchAction(state.initialized && showSearch) {
            LibraryKeyboard.open(viewModel.state.value.query, viewModel::setQuery, searchPlaceholder)
        }
    }
    LaunchedEffect(directories, nativeReady) { viewModel.load(directories, nativeReady) }
    DisposableEffect(viewModel, onOpenMenu) {
        HomeInputController.bind(viewModel, onOpenMenu)
        onDispose { HomeInputController.unbind(viewModel) }
    }

    CompositionLocalProvider(LocalCustomCoverMap provides customCoverMap) {
    ArmsBackdrop(
        // Full-bleed wallpaper: the library image + readability scrim, drawn edge-to-edge
        // (behind the gesture bar) so it never leaves an exposed strip at the bottom —
        // that strip was the "blue bar" in landscape.
        backgroundLayer = {
            val libraryBg = LibraryBackground.uri.value
            if (libraryBg == null) {
                // Default: the live PS3-XMB wave (XmbGlView — a GLES3 port of linkev's
                // grid-displacement mesh, matching iOS). When GL can't init — older Mali without
                // float-texture filtering, or any EGL failure — we fall back to LibraryWaveBackground,
                // a procedural PPSSPP-style animated background drawn on the hardware 2D Canvas (no
                // GLES3, runs anywhere) that reads the SAME colour prefs as the GL wave, so Mali users
                // finally get an animated, recolourable backdrop instead of the old fixed GIF. The
                // static colour-matched fallback stays beneath the GL view during startup and
                // teardown. Custom backgrounds below override all of this.
                if (LibraryBackground.animated2D.value) {
                    // User opted into the lightweight 2D animated wave everywhere (#Luminz) — the same
                    // backdrop GL-fail devices get; skip the GLES3 XmbGlView entirely.
                    LibraryWaveBackground(Modifier.fillMaxSize())
                } else {
                    var xmbGlState by remember { mutableStateOf<Boolean?>(null) } // null=starting, true=up, false=failed
                    // The TextureView can briefly become transparent on launch/rotation.
                    // Keep the user's colours beneath it instead of exposing the bundled blue
                    // image. Freeze the fallback while GL works so there is only one animation.
                    LibraryWaveBackground(Modifier.fillMaxSize(), animated = xmbGlState == false)
                    AndroidView(
                        factory = { XmbGlView(it).apply { onGlStatus = { ok -> xmbGlState = ok } } },
                        modifier = Modifier.fillMaxSize(),
                    )
                }
            } else {
                // User-picked still image / GIF (Coil handles both).
                AsyncImage(
                    model = ImageRequest.Builder(context).data(libraryBg).crossfade(true).build(),
                    contentDescription = null,
                    modifier = Modifier.fillMaxSize(),
                    contentScale = ContentScale.Crop,
                )
            }
            // Scrim so covers and text stay readable over the backdrop. A user-picked image can
            // be any brightness, so it gets the full dark scrim. The XMB is our own controlled
            // backdrop (dark at the top where the content sits) and a heavy scrim just muddied
            // its blue into navy — so it gets only a whisper of dimming, letting the vivid blue
            // read through.
            val scrimTop = if (libraryBg == null) 0.06f else 0.55f
            val scrimBottom = if (libraryBg == null) 0.20f else 0.80f
            Box(
                Modifier.fillMaxSize().background(
                    Brush.verticalGradient(
                        listOf(
                            MaterialTheme.colorScheme.background.copy(alpha = scrimTop),
                            MaterialTheme.colorScheme.background.copy(alpha = scrimBottom),
                        ),
                    ),
                ),
            )
        },
    ) {
        if (collectionsOpen) {
            CollectionsOverlay(state.allGames, collectionGame, embedded = true,
                initialEditingId = editingCollectionId, initialCreate = creatingCollection) {
                val editedIndex = folderEntries.indexOfFirst { it.key == editingCollectionId }
                if (editedIndex >= 0) {
                    HomeInputController.collectionIndex.intValue = editedIndex
                    HomeInputController.zone.value = HomeZone.Collections
                }
                collectionsOpen = false
                collectionGame = null
                editingCollectionId = null
                creatingCollection = false
            }
        } else BoxWithConstraints(modifier.fillMaxSize()) {
            val compact = maxWidth < 600.dp
            // Adaptive cells alone give a tablet MORE columns rather than BIGGER art, so scale the
            // cell width. Bigger cells mean bigger covers and fewer, better-spaced columns. Opt-in:
            // 1.0 everywhere until the user moves the Cover size slider.
            val coverScale = com.armsx2.ui.UiScale.coverScale.value
            val gridCellDp = (if (compact) 104f else 118f) * coverScale
            val columns = if (state.layout == LibraryLayout.Grid) {
                GridCells.Adaptive(gridCellDp.dp)
            } else {
                // List and Shelf are full-width rows.
                GridCells.Fixed(1)
            }
            // Column count feeds HomeInputController's Up/Down step. Shelf view lays
            // covers out in rows of `perShelf`, so it must report that count (not 1) —
            // otherwise Up/Down move one cover at a time (feeling like Left/Right) and
            // only the very first cover can step up into the Recents row.
            val estimatedColumns = when (state.layout) {
                // MUST track gridCellDp — this feeds HomeInputController's Up/Down step, so if the
                // estimate and the real column count diverge, controller navigation skips rows.
                LibraryLayout.Grid ->
                    (maxWidth.value / ((if (compact) 112f else 128f) * coverScale)).toInt().coerceAtLeast(1)
                LibraryLayout.Shelf ->
                    (maxWidth.value / (((if (compact) 84f else 100f) * coverScale) + 20f)).toInt().coerceIn(3, 8)
                LibraryLayout.List -> 1
            }
            LaunchedEffect(estimatedColumns) { HomeInputController.setColumnCount(estimatedColumns) }

            val gridState = rememberLazyGridState()
            // A new query is a new result set. Keeping the previous lazy-grid position can land
            // halfway through that set and hide the strongest matches above the keyboard.
            LaunchedEffect(state.query) {
                if (state.query.isNotBlank()) gridState.scrollToItem(0)
            }
            val density = LocalDensity.current
            LaunchedEffect(gridState) {
                var lastFrame = withFrameNanos { it }
                while (true) {
                    val frame = withFrameNanos { it }
                    val dt = ((frame - lastFrame).coerceAtMost(50_000_000L)).toFloat() / 1_000_000_000f
                    lastFrame = frame
                    val velocity = HomeInputController.scrollVelocity.floatValue
                    if (abs(velocity) > 0.08f) {
                        val pxPerSecond = with(density) { 1500.dp.toPx() }
                        gridState.scrollBy(velocity * pxPerSecond * dt)
                    }
                }
            }
            // The Recently-Played games shown above the grid (empty while searching);
            // register them so the controller's Recents zone can launch them.
            val shownRecents = if (showRecents && state.recentGames.isNotEmpty() && state.query.isBlank()) {
                state.recentGames.take(10)
            } else {
                emptyList()
            }
            LaunchedEffect(shownRecents) {
                HomeInputController.setClearRecentsAction { showClearRecentsConfirm = true }
                HomeInputController.setRecents(shownRecents.size) { i ->
                    shownRecents.getOrNull(i)?.let(viewModel::launch)
                }
            }
            // Camera-follow: while browsing with a controller (Grid zone), keep the
            // selected cover on screen. The grid has leading full-span items (toolbar,
            // search, recents section, All-Games header) before the game cells, so the
            // selected game's lazy-grid index = leading + its row (shelf) / flat index
            // (grid, list). Only scroll when it's actually off-screen so paging through a
            // visible screenful doesn't jitter. (Previously this only ran while searching,
            // so normal browsing never followed the selector.)
            val allGamesHeaderShown = shownRecents.isNotEmpty() &&
                state.initialized && state.visibleGames.isNotEmpty()
            val toolbarBottom = ToolbarPositionPreferences.atBottom.value
            LaunchedEffect(state.selectedIndex, state.visibleGames.size, state.layout, HomeInputController.zone.value,
                state.initialized, showSearch, shownRecents.size, toolbarBottom) {
                if (HomeInputController.zone.value != HomeZone.Grid) return@LaunchedEffect
                // Don't follow (and thus don't scroll away from the top) until the user
                // has actually navigated — otherwise a cold open snaps the grid to the
                // initially-selected cover and hides the toolbar/search/recents header.
                if (!HomeInputController.userNavigated) return@LaunchedEffect
                val sel = state.selectedIndex
                if (sel < 0 || state.visibleGames.isEmpty()) return@LaunchedEffect
                val leading = (if (toolbarBottom) 0 else 1) + // toolbar
                    (if (state.initialized && showSearch) 1 else 0) + // search field
                    (if (state.initialized) 1 else 0) + // collections row
                    (if (shownRecents.isNotEmpty()) 1 else 0) + // recents section
                    (if (allGamesHeaderShown) 1 else 0) // All Games header
                val cols = estimatedColumns.coerceAtLeast(1)
                val target = leading + if (state.layout == LibraryLayout.Shelf) sel / cols else sel
                val vis = gridState.layoutInfo.visibleItemsInfo
                val first = vis.firstOrNull()?.index
                val last = vis.lastOrNull()?.index
                if (first == null || last == null || target < first || target > last) {
                    gridState.animateScrollToItem(target.coerceAtLeast(0))
                }
            }
            // Entering a chrome zone (Toolbar / Search / Recents) scrolls the grid to the
            // very top so those rows are on-screen when focused.
            LaunchedEffect(HomeInputController.zone.value) {
                if (HomeInputController.zone.value != HomeZone.Grid) gridState.animateScrollToItem(0)
            }

            // Keep covers/text out of the display's unsafe edge (cutout / rounded
            // corner) — add the cutout inset to the side padding. The full-bleed
            // shelf/recent rows only negate the base 8dp, so they stop at the safe
            // edge rather than bleeding under the cutout.
            val cutout = WindowInsets.displayCutout.asPaddingValues()
            val ld = LocalLayoutDirection.current

            // Toolbar position is an App setting. At the top it's the first grid item;
            // at the bottom it's a pinned bar (identical rounded-pill shape). The grid
            // reserves the matching inset so nothing hides behind whichever edge it's on.
            LaunchedEffect(toolbarBottom) { HomeInputController.setToolbarAtBottom(toolbarBottom) }
            val statusBarTop = WindowInsets.statusBars.asPaddingValues().calculateTopPadding()
            val navBarBottom = WindowInsets.navigationBars.asPaddingValues().calculateBottomPadding()
            val libraryToolbar: @Composable (Boolean) -> Unit = { bottomEdge ->
                val leadingAction: () -> Unit = if (activeCollection != null) {
                    { HomeInputController.returnToLibrary(); Unit }
                } else onOpenMenu
                // Register the toolbar button actions (left→right order) so the
                // controller's toolbar zone can fire them, and read the highlight
                // state so the focused button lights up.
                HomeInputController.setToolbarActions(
                    listOf(
                        leadingAction,
                        { viewModel.refresh() },
                        { viewModel.toggleLayout() },
                        { overflowMenu = true },
                    ),
                )
                val tb = libraryFocus && HomeInputController.zone.value == HomeZone.Toolbar
                val tbi = HomeInputController.toolbarIndex.intValue
                ArmsTopBar(
                    title = activeCollection?.name ?: str("games.section.library"),
                    subtitle = if (state.scanning) {
                        str("games.scanningRoms")
                    } else {
                        "${str("games.library.totalGames")}: ${if (activeCollection == null) state.allGames.size else state.allGames.count { it.uri.toString() in activeCollection.gameUris }}"
                    },
                    leading = {
                        RoundAction(
                            if (activeCollection != null) "←" else "☰",
                            if (activeCollection != null) str("collections.all") else str("games.overflow.openNavigation"),
                            leadingAction,
                            selected = tb && tbi == 0,
                            framed = true,
                            buttonSize = 44.dp,
                            buttonShape = RoundedCornerShape(14.dp),
                            subtleFrame = true,
                        )
                    },
                    actions = {
                        // Clock + battery, ahead of the buttons so it reads as status rather than
                        // as another control. Deliberately NOT controllerFocusable — it isn't
                        // interactive, and registering it would put a dead stop in the pad's path
                        // through the toolbar.
                        com.armsx2.ui.common.LibraryStatusCluster(
                            // align(): the title block makes the bar taller than this two-line
                            // cluster, so without it the pair sits high relative to the buttons.
                            Modifier.align(Alignment.CenterVertically).padding(end = 6.dp),
                            // Portrait: single compact row so the narrow bar doesn't cram it.
                            compact = LocalConfiguration.current.orientation ==
                                android.content.res.Configuration.ORIENTATION_PORTRAIT,
                        )
                        RoundAction(
                            "↻",
                            str("games.card.refresh"),
                            viewModel::refresh,
                            selected = tb && tbi == 1,
                            framed = false,
                        )
                        RoundAction(
                            when (state.layout) {
                                LibraryLayout.Grid -> "☷"
                                LibraryLayout.List -> "▦"
                                LibraryLayout.Shelf -> "▤"
                            },
                            str("games.toolbar.rows"),
                            viewModel::toggleLayout,
                            selected = tb && tbi == 2,
                            framed = false,
                        )
                        Box {
                            RoundAction(
                                "⋮",
                                str("games.toolbar.more"),
                                { overflowMenu = true },
                                selected = tb && tbi == 3,
                                framed = false,
                            )
                            LibraryOverflowMenu(
                                expanded = overflowMenu,
                                selectedSort = state.sort,
                                use3dCovers = CoverArtStyle.use3d.value,
                                showGridNames = GridLabels.show.value,
                                customNames = com.armsx2.CustomNames.enabled.value,
                                englishTitles = EnglishTitles.enabled.value,
                                showHidden = com.armsx2.HiddenGames.showHidden.value,
                                hasCustomBackground = LibraryBackground.uri.value != null,
                                onDismiss = { overflowMenu = false },
                                onOpenNavigation = onOpenMenu,
                                onSort = viewModel::setSort,
                                onCollections = { foldersExpanded = true; com.armsx2.data.library.GameCollections.select(null) },
                                onToggleCoverStyle = { CoverArtStyle.set(!CoverArtStyle.use3d.value) },
                                onToggleGridNames = { GridLabels.set(!GridLabels.show.value) },
                                onToggleCustomNames = { com.armsx2.CustomNames.set(!com.armsx2.CustomNames.enabled.value) },
                                onToggleEnglishTitles = { EnglishTitles.set(!EnglishTitles.enabled.value) },
                                onToggleShowHidden = { viewModel.setShowHidden(!com.armsx2.HiddenGames.showHidden.value) },
                                onChooseBackground = { backgroundPicker.launch(arrayOf("image/*")) },
                                onClearBackground = LibraryBackground::clear,
                                onExitApp = { showExitConfirm = true },
                            )
                            if (showExitConfirm) {
                                AlertDialog(
                                    onDismissRequest = { showExitConfirm = false },
                                    title = { Text(str("games.exit.title")) },
                                    text = { Text(str("games.exit.message")) },
                                    confirmButton = {
                                        TextButton(modifier = Modifier.padFocusRing(), onClick = {
                                            showExitConfirm = false
                                            MainActivityRuntime.exitApp()
                                        }) { Text(str("games.toolbar.exit")) }
                                    },
                                    dismissButton = {
                                        TextButton(modifier = Modifier.padFocusRing(), onClick = { showExitConfirm = false }) {
                                            Text(str("action.cancel"))
                                        }
                                    },
                                )
                            }
                        }
                    },
                    horizontalPadding = 0.dp,
                    bottomEdge = bottomEdge,
                )
            }
            LazyVerticalGrid(
                columns = columns,
                state = gridState,
                modifier = Modifier.fillMaxSize(),
                contentPadding = PaddingValues(
                    start = 8.dp + cutout.calculateStartPadding(ld),
                    end = 8.dp + cutout.calculateEndPadding(ld),
                    top = if (toolbarBottom) statusBarTop + 8.dp else 0.dp,
                    bottom = if (toolbarBottom) navBarBottom + 72.dp else 16.dp,
                ),
                horizontalArrangement = Arrangement.spacedBy(9.dp),
                verticalArrangement = Arrangement.spacedBy(12.dp),
            ) {
                if (!toolbarBottom) {
                    item(span = { GridItemSpan(maxLineSpan) }) { libraryToolbar(false) }
                }
                if (state.initialized && showSearch) {
                    item(span = { GridItemSpan(maxLineSpan) }) {
                        SearchField(
                            value = state.query,
                            onClick = { LibraryKeyboard.open(viewModel.state.value.query, viewModel::setQuery, searchPlaceholder) },
                            placeholder = searchPlaceholder,
                            modifier = Modifier.fillMaxWidth(),
                            selected = libraryFocus && HomeInputController.zone.value == HomeZone.Search,
                        )
                    }
                }

                if (state.initialized) {
                    item(key = "home-collections", span = { GridItemSpan(maxLineSpan) }) {
                        val focused = libraryFocus && HomeInputController.zone.value == HomeZone.Collections
                        Column {
                            Row(verticalAlignment = Alignment.CenterVertically) {
                                SectionTitle(str("collections.title"), modifier = Modifier.weight(1f))
                                if (focused) {
                                    folderEntries.getOrNull(HomeInputController.collectionIndex.intValue)?.edit?.let { edit ->
                                        TextButton(onClick = { HomeInputController.finishTileMove(); edit() }) {
                                            Text(str("collections.edit"))
                                        }
                                    }
                                }
                            }
                            if (focused && HomeInputController.movingTile.value != null) {
                                Text(str("collections.moveControls"), style = MaterialTheme.typography.labelMedium)
                                Row {
                                    TextButton(onClick = { HomeInputController.moveTile(-1) }) { Text("←") }
                                    TextButton(onClick = { HomeInputController.moveTile(1) }) { Text("→") }
                                    TextButton(onClick = { HomeInputController.finishTileMove() }) { Text(str("collections.done")) }
                                }
                            }
                            Spacer(Modifier.height(9.dp))
                            val rowState = rememberLazyListState()
                            val index = HomeInputController.collectionIndex.intValue
                            LaunchedEffect(index, focused, folderEntries.size) {
                                if (focused) rowState.animateScrollToItem(index.coerceIn(0, folderEntries.lastIndex))
                            }
                            LazyRow(state = rowState, horizontalArrangement = Arrangement.spacedBy(12.dp)) {
                                itemsIndexed(folderEntries, key = { _, item -> item.key }) { i, item ->
                                    if (item.game != null) RecentGameCard(item.game, focused && index == i,
                                        onClick = item.action, onDetails = { item.edit?.invoke() })
                                    else CollectionHomeTile(focused && index == i, label = item.label,
                                        onEdit = item.edit, glyph = item.glyph, expanded = item.expanded,
                                        artworkKey = item.key,
                                        moving = HomeInputController.movingTile.value == item.key,
                                        onBeginMove = if (item.key in com.armsx2.data.library.GameCollections.homeTileKeys())
                                            { { HomeInputController.beginTileMove(item.key) } } else null,
                                        onMove = { direction -> HomeInputController.moveTile(direction) },
                                        onClick = {
                                            if (HomeInputController.movingTile.value != null) HomeInputController.finishTileMove()
                                            else item.action()
                                        })
                                }
                            }
                        }
                    }
                }

                if (shownRecents.isNotEmpty()) {
                    val recentsSelected = libraryFocus && HomeInputController.zone.value == HomeZone.Recents
                    val recentSel = if (recentsSelected) HomeInputController.recentIndex.intValue else -1
                    item(span = { GridItemSpan(maxLineSpan) }) {
                        Column {
                            Row(verticalAlignment = Alignment.CenterVertically) {
                                // In shelf view nudge the header right so it lines up with
                                // the first cover's left edge (the shelf's 12dp inset).
                                SectionTitle(
                                    str("games.section.recentlyPlayed"),
                                    modifier = Modifier.padding(
                                        start = if (state.layout == LibraryLayout.Shelf) 4.dp else 0.dp,
                                    ),
                                )
                                Spacer(Modifier.width(10.dp))
                                val clearAll = { showClearRecentsConfirm = true }
                                Surface(
                                    onClick = clearAll,
                                    modifier = Modifier.selectionOutline(
                                        libraryFocus && HomeInputController.zone.value == HomeZone.RecentsHeader,
                                        RoundedCornerShape(12.dp),
                                    ),
                                    shape = RoundedCornerShape(12.dp),
                                    color = MaterialTheme.colorScheme.surfaceVariant.copy(alpha = 0.7f),
                                ) {
                                    Text(
                                        str("games.recent.clearAll"),
                                        modifier = Modifier.padding(horizontal = 12.dp, vertical = 5.dp),
                                        style = MaterialTheme.typography.labelMedium,
                                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                                    )
                                }
                            }
                            Spacer(Modifier.height(9.dp))
                            if (state.layout == LibraryLayout.Shelf) {
                                // Same frosted-glass plank as All Games (bagas: one
                                // shelf design everywhere).
                                GameShelf(
                                    games = shownRecents,
                                    shelfRes = R.drawable.shelf_frosted,
                                    coverWidth = ((if (compact) 84f else 100f) * coverScale).dp,
                                    scroll = true,
                                    selectedIndex = recentSel,
                                    onLaunch = { viewModel.launch(it) },
                                    onDetails = { menuGame = it },
                                    modifier = Modifier.layout { measurable, constraints ->
                                        val edge = 8.dp.roundToPx()
                                        val placeable = measurable.measure(
                                            constraints.copy(
                                                minWidth = constraints.maxWidth + edge * 2,
                                                maxWidth = constraints.maxWidth + edge * 2,
                                            ),
                                        )
                                        layout(constraints.maxWidth, placeable.height) { placeable.placeRelative(-edge, 0) }
                                    },
                                )
                            } else {
                                val recentsRowState = rememberLazyListState()
                                LaunchedEffect(recentSel) {
                                    if (recentSel >= 0) recentsRowState.animateScrollToItem(recentSel)
                                }
                                LazyRow(
                                    state = recentsRowState,
                                    modifier = Modifier
                                        .fillMaxWidth()
                                        .layout { measurable, constraints ->
                                            val edge = 8.dp.roundToPx()
                                            val placeable = measurable.measure(
                                                constraints.copy(
                                                    minWidth = constraints.minWidth + edge * 2,
                                                    maxWidth = constraints.maxWidth + edge * 2,
                                                ),
                                            )
                                            layout(constraints.maxWidth, placeable.height) {
                                                placeable.placeRelative(-edge, 0)
                                            }
                                        },
                                    horizontalArrangement = Arrangement.spacedBy(10.dp),
                                    contentPadding = PaddingValues(horizontal = 8.dp),
                                ) {
                                    itemsIndexed(shownRecents, key = { _, g -> g.uri.toString() }) { index, game ->
                                        RecentGameCard(
                                            game = game,
                                            selected = index == recentSel,
                                            onClick = { viewModel.launch(game) },
                                            onDetails = { menuGame = game },
                                        )
                                    }
                                }
                            }
                        }
                    }
                }

                // Separate the Recently Played shelf from the full library with an
                // "All Games" header so the two rows don't run together.
                if (shownRecents.isNotEmpty() && state.initialized && state.visibleGames.isNotEmpty()) {
                    item(span = { GridItemSpan(maxLineSpan) }) {
                        HorizontalDivider(
                            modifier = Modifier.padding(vertical = 8.dp),
                            color = MaterialTheme.colorScheme.outline.copy(alpha = 0.34f),
                        )
                    }
                }

                if (!state.initialized) {
                    item(span = { GridItemSpan(maxLineSpan) }) {
                        Box(Modifier.fillMaxWidth().height(240.dp), contentAlignment = Alignment.Center) {
                            CircularProgressIndicator()
                        }
                    }
                } else if (state.visibleGames.isEmpty()) {
                    emptyLibrary(state.query.isBlank(), activeCollection != null) { toggleFolders() }
                } else if (state.layout == LibraryLayout.Shelf) {
                    // Fill each plank: chunk by how many covers fit the shelf width.
                    val shelfCoverW = ((if (compact) 84f else 100f) * coverScale).dp
                    val perShelf = (maxWidth.value / (shelfCoverW.value + 20f)).toInt().coerceIn(3, 8)
                    val shelfRows = state.visibleGames.chunked(perShelf)
                    items(
                        shelfRows.size,
                        span = { GridItemSpan(maxLineSpan) },
                        key = { "shelf_$it" },
                    ) { rowIndex ->
                        GameShelf(
                            games = shelfRows[rowIndex],
                            shelfRes = R.drawable.shelf_frosted,
                            coverWidth = shelfCoverW,
                            scroll = false,
                            // Every row lays out on the same perShelf-slot grid, so a
                            // short last row keeps its covers packed left in sequence.
                            slotsPerRow = perShelf,
                            selectedIndex = if (gridFocus) state.selectedIndex else -1,
                            startIndex = rowIndex * perShelf,
                            onLaunch = { viewModel.launch(it) },
                            onDetails = { menuGame = it },
                            // Bleed past the grid's 8dp side padding so the glass shelf
                            // reaches both screen edges instead of floating inset.
                            modifier = Modifier.layout { measurable, constraints ->
                                val edge = 8.dp.roundToPx()
                                val placeable = measurable.measure(
                                    constraints.copy(
                                        minWidth = constraints.maxWidth + edge * 2,
                                        maxWidth = constraints.maxWidth + edge * 2,
                                    ),
                                )
                                layout(constraints.maxWidth, placeable.height) { placeable.placeRelative(-edge, 0) }
                            },
                        )
                    }
                } else {
                    itemsIndexed(
                        state.visibleGames,
                        key = { _, game -> game.uri.toString() },
                        contentType = { _, _ -> state.layout.name },
                    ) { index, game ->
                        if (state.layout == LibraryLayout.Grid) {
                            GameGridCard(
                                game = game,
                                selected = gridFocus && index == state.selectedIndex,
                                onSelect = { viewModel.setSelection(index) },
                                onLaunch = { viewModel.launch(game) },
                                onDetails = { menuGame = game },
                            )
                        } else {
                            GameListCard(
                                game = game,
                                selected = gridFocus && index == state.selectedIndex,
                                onClick = { viewModel.setSelection(index); viewModel.launch(game) },
                                onDetails = { menuGame = game },
                            )
                        }
                    }
                }
            }

            // Pinned bottom toolbar (App setting) — same rounded-pill component as the
            // top placement. Match the top's width by applying the SAME side inset the
            // grid's contentPadding gives the top bar (8dp + display cutout); otherwise
            // the bottom bar spans edge-to-edge and reads wider than the top.
            if (toolbarBottom) {
                Box(
                    Modifier
                        .align(Alignment.BottomCenter)
                        .fillMaxWidth()
                        .padding(
                            start = 8.dp + cutout.calculateStartPadding(ld),
                            end = 8.dp + cutout.calculateEndPadding(ld),
                        ),
                ) {
                    libraryToolbar(true)
                }
            }
        }
        // The keyboard is hosted once in WindowImpl, above every surface — hosting it here as
        // well would draw it twice, and a per-screen host is what made it invisible from
        // Settings (a nav destination that unmounts this screen).
    }
    }

    // Sits outside the grid so it draws over the whole library. Not an AlertDialog — a Compose
    // dialog is its own window and would swallow the D-pad, and this has to be pad-navigable.
    if (showClearRecentsConfirm) {
        com.armsx2.ui.common.ConfirmOverlay(
            title = str("games.recent.clearAll.title"),
            message = str("games.recent.clearAll.message"),
            confirmLabel = str("games.recent.clearAll"),
            destructive = true,
            idPrefix = "clear-recents",
            onConfirm = {
                viewModel.clearRecent()
                showClearRecentsConfirm = false
            },
            onDismiss = { showClearRecentsConfirm = false },
        )
    }

    menuGame?.let { game ->
        // Tri-state on purpose: null while identifying, blank when the image cannot be identified.
        // produceState alone cannot tell those apart — both are null — so an unidentifiable game
        // would sit on "…" forever, reading as still-loading when it is actually unknown.
        val menuCRC by androidx.compose.runtime.produceState<String?>(initialValue = null, game.uri) {
            value = com.armsx2.DiscIdentity.resolve(game.uri, game.serial) ?: ""
        }
        ModalBottomSheet(onDismissRequest = { menuGame = null }) {
            Column(
                Modifier.fillMaxWidth().verticalScroll(rememberScrollState()).padding(start = 8.dp, end = 8.dp, bottom = 20.dp),
                verticalArrangement = Arrangement.spacedBy(8.dp),
            ) {
                Text(
                    game.displayTitle(EnglishTitles.enabled.value),
                    style = MaterialTheme.typography.titleLarge,
                    maxLines = 2,
                    overflow = TextOverflow.Ellipsis,
                    modifier = Modifier.padding(horizontal = 8.dp, vertical = 6.dp),
                )
                // Serial and CRC — the two halves of a <SERIAL>_<CRC>.pnach filename.
                val identity = buildList {
                    game.serial?.takeIf { it.isNotBlank() }?.let(::add)
                    add("CRC " + when (menuCRC) {
                        null -> "…"   // still identifying
                        "" -> "—"     // identified as unknown
                        else -> menuCRC
                    })
                }.joinToString("  ·  ")
                Text(
                    identity,
                    style = MaterialTheme.typography.bodyMedium,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                    modifier = Modifier.padding(horizontal = 8.dp),
                )
                GameMenuAction("▶", str("action.play")) {
                    menuGame = null
                    viewModel.launch(game)
                }
                GameMenuAction("▤", str("collections.title")) {
                    menuGame = null
                    openCollections(game = game)
                }
                GameMenuAction("⚙", str("action.settings")) {
                    menuGame = null
                    onOpenGameSettings(game)
                }
                // Per-game BIOS: open the BIOS manager scoped to THIS game (no need to load it),
                // since the BIOS manager isn't reachable from the in-game menu.
                GameMenuAction("📀", str("bios.perGame.menu")) {
                    menuGame = null
                    com.armsx2.navigation.UiNavigator.navigate(com.armsx2.navigation.AppRoute.BiosManager(game))
                }
                // Cheats for THIS game, without loading it. A cheat selection belongs to one
                // disc, so the game's own menu is where people look for it — and it is the
                // reason this is here as well as on the settings hub's Cheats tab: a feature
                // reachable only from an in-game menu is a feature users report as missing.
                GameMenuAction("✦", str("cheats.title")) {
                    menuGame = null
                    com.armsx2.navigation.UiNavigator.navigate(
                        com.armsx2.navigation.AppRoute.Settings(
                            com.armsx2.navigation.SettingsCategory.Patches,
                            game,
                        ),
                    )
                }
                // Pin to the launcher (issue #242). The action was lost when this menu was
                // rebuilt, leaving HomeShortcuts with no call site at all (issue #335).
                // pin() returns false only when the launcher can't pin — surface that.
                val addToHomeFailed = str("games.addToHome.unsupported")
                GameMenuAction("📌", str("games.addToHome")) {
                    menuGame = null
                    if (!com.armsx2.HomeShortcuts.pin(context, game))
                        Toast.makeText(context, addToHomeFailed, Toast.LENGTH_LONG).show()
                }
                // Only offered when the game is actually in Recently Played — this drops
                // just this one entry, unlike the library-wide "Show Recently Played" toggle.
                if (state.recentGames.any { it.uri == game.uri }) {
                    GameMenuAction("🕐", str("games.removeRecent")) {
                        viewModel.removeFromRecent(game)
                        menuGame = null
                    }
                }
                val hidden = com.armsx2.HiddenGames.isHidden(game)
                GameMenuAction(if (hidden) "◍" else "🚫", str(if (hidden) "games.unhide" else "games.hide")) {
                    viewModel.setHidden(game, !hidden)
                    menuGame = null
                }
            }
        }
    }
}

@Composable
private fun GameMenuAction(glyph: String, label: String, onClick: () -> Unit) {
    Surface(
        onClick = onClick,
        modifier = Modifier.fillMaxWidth().padFocusRing(RoundedCornerShape(18.dp)),
        shape = RoundedCornerShape(18.dp),
        color = MaterialTheme.colorScheme.surfaceVariant,
        border = BorderStroke(1.dp, MaterialTheme.colorScheme.outline.copy(alpha = 0.45f)),
    ) {
        Row(
            Modifier.fillMaxWidth().padding(horizontal = 16.dp, vertical = 14.dp),
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.spacedBy(14.dp),
        ) {
            Text(glyph, color = MaterialTheme.colorScheme.primary, fontSize = 20.sp)
            Text(label, style = MaterialTheme.typography.titleMedium)
        }
    }
}

@Composable
private fun LibraryOverflowMenu(
    expanded: Boolean,
    selectedSort: HomeSort,
    use3dCovers: Boolean,
    showGridNames: Boolean,
    customNames: Boolean,
    englishTitles: Boolean,
    showHidden: Boolean,
    hasCustomBackground: Boolean,
    onDismiss: () -> Unit,
    onOpenNavigation: () -> Unit,
    onSort: (HomeSort) -> Unit,
    onCollections: () -> Unit,
    onToggleCoverStyle: () -> Unit,
    onToggleGridNames: () -> Unit,
    onToggleCustomNames: () -> Unit,
    onToggleEnglishTitles: () -> Unit,
    onToggleShowHidden: () -> Unit,
    onChooseBackground: () -> Unit,
    onClearBackground: () -> Unit,
    onExitApp: () -> Unit,
) {
    fun closeThen(action: () -> Unit) {
        onDismiss()
        action()
    }

    DropdownMenu(
        expanded = expanded,
        onDismissRequest = onDismiss,
        modifier = Modifier.widthIn(min = 320.dp, max = 380.dp),
        shape = RoundedCornerShape(22.dp),
        containerColor = MaterialTheme.colorScheme.surface,
        tonalElevation = 8.dp,
        shadowElevation = 14.dp,
        border = BorderStroke(1.dp, MaterialTheme.colorScheme.outline.copy(alpha = 0.42f)),
    ) {
        Text(
            text = str("games.section.library"),
            modifier = Modifier.padding(horizontal = 18.dp, vertical = 10.dp),
            style = MaterialTheme.typography.labelLarge,
            color = MaterialTheme.colorScheme.primary,
            fontWeight = FontWeight.Bold,
        )
        LibraryOverflowItem("▤", str("collections.title")) { closeThen(onCollections) }
        OverflowSeparator()
        LibraryOverflowItem(
            glyph = "A–Z",
            label = str("games.overflow.sortTitle"),
            selected = selectedSort == HomeSort.Title,
        ) {
            closeThen { onSort(HomeSort.Title) }
        }
        LibraryOverflowItem(
            glyph = "⇅",
            label = str("games.overflow.sortRecent"),
            selected = selectedSort == HomeSort.RecentlyPlayed,
        ) {
            closeThen { onSort(HomeSort.RecentlyPlayed) }
        }
        OverflowSeparator()
        LibraryOverflowItem(
            glyph = if (use3dCovers) "3D" else "2D",
            label = str("games.overflow.coverStyle"),
            trailing = if (use3dCovers) "3D" else "2D",
        ) {
            closeThen(onToggleCoverStyle)
        }
        LibraryOverflowItem(
            glyph = "Aa",
            label = str("games.overflow.customNames"),
            trailing = if (customNames) str("common.on") else str("common.off"),
        ) {
            closeThen(onToggleCustomNames)
        }
        LibraryOverflowItem(
            glyph = "A/あ",
            label = str("games.overflow.englishTitles"),
            trailing = if (englishTitles) str("common.on") else str("common.off"),
        ) {
            closeThen(onToggleEnglishTitles)
        }
        LibraryOverflowItem(
            glyph = "◍",
            label = str("games.overflow.showHidden"),
            trailing = if (showHidden) str("common.on") else str("common.off"),
        ) {
            closeThen(onToggleShowHidden)
        }
        OverflowSeparator()
        LibraryOverflowItem("▧", str("games.background.choose")) {
            closeThen(onChooseBackground)
        }
        if (hasCustomBackground) {
            LibraryOverflowItem("×", str("games.background.clear")) {
                closeThen(onClearBackground)
            }
        }
        OverflowSeparator()
        // Exit, back where it used to live. It moved to the drawer, which put it below every other
        // destination -- so quitting, one of the most frequent things anyone does here, meant
        // opening the drawer and scrolling to the bottom every time (issue #460, and shinobumaehara
        // is right that frequency should decide placement). It stays in the drawer too; this is the
        // short path, not a replacement.
        //
        // The confirmation is the point of the row and travels with it: quitting mid-session
        // without one loses whatever is not saved.
        // onExitApp was still a parameter and its confirmation dialog was still wired up — only
        // the row that reached them had been removed. So this restores the item, not the feature.
        LibraryOverflowItem(
            glyph = "⏻",
            label = str("games.toolbar.exit"),
            iconRes = com.armsx2.R.drawable.ic_power,
            iconTint = Color(0xFFE60012),
        ) {
            closeThen(onExitApp)
        }
    }
}

@Composable
private fun OverflowSeparator() {
    androidx.compose.material3.HorizontalDivider(
        modifier = Modifier.padding(horizontal = 18.dp, vertical = 6.dp),
        color = MaterialTheme.colorScheme.outline.copy(alpha = 0.30f),
    )
}

@Composable
private fun LibraryOverflowItem(
    glyph: String,
    label: String,
    selected: Boolean = false,
    trailing: String? = null,
    // A real drawable instead of a text glyph. Exit needs this: the power symbol (U+23FB) is not
    // in the bundled font and rendered as a tofu box. Null keeps the glyph path for every other row.
    iconRes: Int? = null,
    iconTint: Color? = null,
    onClick: () -> Unit,
) {
    DropdownMenuItem(
        text = {
            Text(
                text = label,
                fontWeight = if (selected) FontWeight.Bold else FontWeight.Medium,
                maxLines = 2,
                lineHeight = 20.sp,
                overflow = TextOverflow.Ellipsis,
            )
        },
        onClick = onClick,
        leadingIcon = {
            Surface(
                modifier = Modifier.size(34.dp),
                shape = RoundedCornerShape(11.dp),
                color = if (selected) MaterialTheme.colorScheme.primaryContainer else MaterialTheme.colorScheme.surfaceVariant,
            ) {
                Box(contentAlignment = Alignment.Center) {
                    if (iconRes != null) {
                        androidx.compose.material3.Icon(
                            painter = androidx.compose.ui.res.painterResource(iconRes),
                            contentDescription = null,
                            tint = iconTint ?: MaterialTheme.colorScheme.onSurface,
                            modifier = Modifier.size(19.dp),
                        )
                    } else {
                        Text(
                            text = glyph,
                            fontSize = if (glyph.length > 2) 11.sp else 17.sp,
                            fontWeight = FontWeight.Bold,
                            color = if (selected) MaterialTheme.colorScheme.onPrimaryContainer else MaterialTheme.colorScheme.onSurface,
                        )
                    }
                }
            }
        },
        trailingIcon = {
            when {
                selected -> Text("✓", color = MaterialTheme.colorScheme.primary, fontWeight = FontWeight.Bold)
                trailing != null -> Text(trailing, color = MaterialTheme.colorScheme.onSurfaceVariant, fontWeight = FontWeight.Bold)
            }
        },
        modifier = Modifier.padding(horizontal = 6.dp).padFocusRing(),
    )
}

private fun LazyGridScope.emptyLibrary(noFolders: Boolean, collection: Boolean = false, onCollections: () -> Unit = {}) {
    item(span = { GridItemSpan(maxLineSpan) }) {
        EmptyState(
            title = if (collection) str("collections.empty") else if (noFolders) str("games.empty.noFolders.title") else str("games.search.placeholder"),
            message = if (collection) str("collections.emptyHint") else if (noFolders) str("games.empty.noFolders.body") else str("games.search.hint"),
            actionLabel = if (collection) str("collections.title") else if (noFolders) str("games.toolbar.setup") else null,
            onAction = if (collection) onCollections else if (noFolders) MainActivityRuntime::reopenSetup else null,
            modifier = Modifier.fillMaxWidth().height(260.dp),
        )
    }
}

@OptIn(ExperimentalFoundationApi::class)
@Composable
private fun GameGridCard(
    game: GameInfo,
    selected: Boolean,
    onSelect: () -> Unit,
    onLaunch: () -> Unit,
    onDetails: () -> Unit,
) {
    Column(
        modifier = Modifier
            .fillMaxWidth()
            .combinedClickable(onClick = { onSelect(); onLaunch() }, onLongClick = onDetails),
    ) {
        GameCover(
            game,
            Modifier
                .fillMaxWidth()
                .aspectRatio(coverAspectRatio())
                .coverFrame(selected, 2.5.dp),
        )
        if (GridLabels.show.value) {
            Spacer(Modifier.height(4.dp))
            Text(
                game.displayTitle(EnglishTitles.enabled.value),
                style = MaterialTheme.typography.labelSmall,
                maxLines = 1,
                overflow = TextOverflow.Ellipsis,
                modifier = Modifier.padding(horizontal = 2.dp),
            )
        }
    }
}

@OptIn(ExperimentalFoundationApi::class)
@Composable
private fun GameListCard(game: GameInfo, selected: Boolean, onClick: () -> Unit, onDetails: () -> Unit) {
    Surface(
        modifier = Modifier.fillMaxWidth().combinedClickable(onClick = onClick, onLongClick = onDetails),
        shape = RoundedCornerShape(15.dp),
        color = MaterialTheme.colorScheme.surface.copy(alpha = LibraryChromePreferences.libraryOpacity.value / 100f),
        border = BorderStroke(
            if (selected) 2.dp else 1.dp,
            if (selected) SelectionBlue else MaterialTheme.colorScheme.outline.copy(alpha = 0.42f),
        ),
    ) {
        Row(Modifier.padding(7.dp), verticalAlignment = Alignment.CenterVertically) {
            GameCover(game, Modifier.width(54.dp).aspectRatio(coverAspectRatio()))
            Spacer(Modifier.width(12.dp))
            Column(Modifier.weight(1f)) {
                Text(game.displayTitle(EnglishTitles.enabled.value), style = MaterialTheme.typography.titleSmall, maxLines = 1, overflow = TextOverflow.Ellipsis)
                Spacer(Modifier.height(5.dp))
                GameMetadata(game)
            }
            Text("▶", color = MaterialTheme.colorScheme.primary, modifier = Modifier.padding(10.dp))
        }
    }
}

@OptIn(ExperimentalFoundationApi::class)
@Composable
private fun RecentGameCard(game: GameInfo, selected: Boolean = false, onClick: () -> Unit, onDetails: () -> Unit) {
    Column(
        modifier = Modifier
            .width(102.dp)
            .combinedClickable(onClick = onClick, onLongClick = onDetails),
    ) {
        GameCover(
            game,
            Modifier.fillMaxWidth().aspectRatio(coverAspectRatio())
                .coverFrame(selected, 2.5.dp),
        )
        Spacer(Modifier.height(5.dp))
        Text(game.displayTitle(EnglishTitles.enabled.value), style = MaterialTheme.typography.labelMedium, maxLines = 1, overflow = TextOverflow.Ellipsis)
    }
}

/** Cover-sized folders shared by Home shortcuts and the full-page collections browser. */
@OptIn(ExperimentalFoundationApi::class)
@Composable
internal fun CollectionHomeTile(selected: Boolean, label: String? = null,
    onEdit: (() -> Unit)? = null, glyph: String = "▤", expanded: Boolean? = null,
    artworkKey: String? = null, moving: Boolean = false,
    onBeginMove: (() -> Unit)? = null, onMove: ((Int) -> Unit)? = null, onClick: () -> Unit) {
    val artworkVersion = com.armsx2.data.library.FolderArtwork.version.intValue
    val artwork = remember(artworkKey, artworkVersion) {
        artworkKey?.let(com.armsx2.data.library.FolderArtwork::image)
    }
    var artworkFailed by remember(artwork) { mutableStateOf(false) }
    val beginMove by rememberUpdatedState(onBeginMove)
    val moveTile by rememberUpdatedState(onMove)
    val drag = if (onBeginMove != null) Modifier.pointerInput(artworkKey) {
        var distance = 0f
        detectDragGesturesAfterLongPress(
            onDragStart = { distance = 0f; beginMove?.invoke() },
            onDrag = { change, amount ->
                change.consume()
                distance += amount.x
                val step = 64.dp.toPx()
                while (abs(distance) >= step) {
                    val direction = if (distance > 0) 1 else -1
                    moveTile?.invoke(direction)
                    distance -= direction * step
                }
            },
            onDragEnd = { distance = 0f }, onDragCancel = { distance = 0f })
    } else Modifier
    Column(Modifier.width(102.dp).then(drag).combinedClickable(onClick = onClick,
        onLongClick = if (onBeginMove == null) onEdit else null)) {
        val primary = MaterialTheme.colorScheme.primaryContainer
        val secondary = MaterialTheme.colorScheme.secondaryContainer
        Box(Modifier.fillMaxWidth().aspectRatio(coverAspectRatio()).coverFrame(selected, 2.5.dp)
            .clip(RoundedCornerShape(12.dp))
            .background(Brush.linearGradient(listOf(primary, secondary)))
            .then(if (moving) Modifier.border(3.dp, SelectionBlue, RoundedCornerShape(12.dp)) else Modifier),
            contentAlignment = Alignment.Center) {
            if (artwork != null && !artworkFailed) {
                AsyncImage(model = artwork, contentDescription = label ?: str("collections.title"),
                    modifier = Modifier.fillMaxSize(), contentScale = ContentScale.Crop,
                    onError = { artworkFailed = true })
                Box(Modifier.fillMaxSize().background(Brush.verticalGradient(listOf(
                    Color.Transparent, Color.Black.copy(alpha = 0.75f)))))
                Text(label ?: str("collections.title"),
                    Modifier.align(Alignment.BottomCenter).padding(horizontal = 8.dp, vertical = 14.dp),
                    style = MaterialTheme.typography.labelMedium, fontWeight = FontWeight.Bold,
                    textAlign = TextAlign.Center, maxLines = 2, overflow = TextOverflow.Ellipsis, color = Color.White)
            }
            expanded?.let {
                Text(if (it) "−" else "+", Modifier.align(Alignment.TopEnd).padding(end = 8.dp),
                    fontSize = 24.sp, color = if (artwork != null && !artworkFailed) Color.White else MaterialTheme.colorScheme.onPrimaryContainer)
            }
            if (artwork == null || artworkFailed) Column(horizontalAlignment = Alignment.CenterHorizontally,
                modifier = Modifier.padding(horizontal = 8.dp),
                verticalArrangement = Arrangement.spacedBy(3.dp)) {
                Text(glyph, fontSize = 24.sp, color = MaterialTheme.colorScheme.onPrimaryContainer)
                Spacer(Modifier.height(16.dp))
                Text(label ?: str("collections.title"), style = MaterialTheme.typography.labelMedium,
                    fontWeight = FontWeight.Bold, textAlign = TextAlign.Center, maxLines = 2,
                    overflow = TextOverflow.Ellipsis, color = MaterialTheme.colorScheme.onPrimaryContainer)
            }
        }
        Spacer(Modifier.height(5.dp))
        Text(label ?: str("collections.title"), style = MaterialTheme.typography.labelMedium,
            maxLines = 1, overflow = TextOverflow.Ellipsis)
    }
}

@Composable
private fun GameMetadata(game: GameInfo) {
    Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(5.dp)) {
        StatusChip(game.extension.ifBlank { game.platform.key.uppercase() })
        game.regionFlag?.let { Text(it, fontSize = 13.sp) }
        if (game.compatibility > 0) {
            Text("★".repeat(game.compatibility), color = Color(0xFFFFC857), fontSize = 9.sp, maxLines = 1)
        }
        // Playtime and last-known achievement progress, shown only when there is something to show
        // so an untouched library looks exactly as before. Playtime is app-side per serial; the
        // achievement counts are whatever the game last reported while running (the core cannot be
        // asked about a game it has not loaded).
        val rev = com.armsx2.PlayTime.revision.value
        val played = remember(game.serial, rev) {
            com.armsx2.PlayTime.formatPlayed(com.armsx2.PlayTime.playedSeconds(game.serial))
        }
        if (played.isNotEmpty()) {
            Text("⏳ $played", color = MaterialTheme.colorScheme.onSurfaceVariant, fontSize = 11.sp, maxLines = 1)
        }
        val ach = remember(game.serial, rev) { com.armsx2.PlayTime.achievements(game.serial) }
        ach?.let { p ->
            // Hardcore and softcore are different accomplishments on RetroAchievements — hardcore
            // forbids save states, cheats and slow motion — so they get different treatment rather
            // than being merged into one number. Lead with hardcore when any exists (it is the
            // stricter figure), and only mention softcore separately when it is actually ahead.
            val leadHardcore = p.hardcore > 0
            val shown = if (leadHardcore) p.hardcore else p.softcore
            val mastered = if (leadHardcore) p.masteredHardcore else p.masteredSoftcore
            Text(
                "🏆 $shown/${p.total}",
                color = when {
                    mastered && leadHardcore -> Color(0xFFFFC857)  // gold: mastered in hardcore
                    mastered -> Color(0xFFB9C2CC)                  // silver: completed in softcore
                    else -> MaterialTheme.colorScheme.onSurfaceVariant
                },
                fontSize = 11.sp,
                maxLines = 1,
            )
            if (leadHardcore) {
                Text(
                    "HC",
                    color = Color(0xFFFFC857),
                    fontSize = 9.sp,
                    fontWeight = FontWeight.Bold,
                    maxLines = 1,
                )
            }
            // Softcore ahead of hardcore: show what is still only earned in casual mode, so the gap
            // is visible instead of the row silently under-reporting the collection.
            if (leadHardcore && p.softcore > p.hardcore) {
                Text(
                    "+${p.softcore - p.hardcore} SC",
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                    fontSize = 9.sp,
                    maxLines = 1,
                )
            }
        }
    }
}

/**
 * Aspect ratio of a cover slot, matched to the artwork actually served.
 *
 * xlenore's 3D case renders are 567x878 (0.646) while the flat 2D scans are 512x736 (0.696). The
 * slot was hardcoded to 0.72 for both, so with ContentScale.Fit the 3D art — being narrower than
 * its slot — sat with transparent margins down each side: the "poorly filled square" that 2D does
 * not show, because 0.696 all but fills 0.72. Reported by Isshin.
 */
@Composable
/**
 * PS1 box art is SQUARE — 1:1 in both styles. Verified against the source repo, not assumed:
 * xlenore/psx-covers serves `covers/default/<serial>.jpg` at 500x500 and `covers/3d/<serial>.png`
 * at 226x226. A jewel case is square, unlike the tall DVD case a PS2 cover comes in.
 *
 * These were 0.72f / 0.646f, inherited straight from ARMSX2 where they are correct. Fitting a
 * square image into a slot that tall left empty bands above and below it, and because the idle
 * frame is drawn around the SLOT rather than the artwork, the frame stood well clear of the
 * cover — the "too much empty space outline" reported on device.
 */
private fun coverAspectRatio(): Float = 1f

/**
 * Frame around a cover slot.
 *
 * The idle 1dp frame only makes sense for the flat 2D scans, which fill their slot as a rectangle
 * so the frame hugs the artwork. A 3D case render is transparent around the angled case, so the
 * same frame draws a rounded rectangle through empty space beside it — the stray outline and edge
 * lines reported in grid view. The shelf never showed them because it frames only the selected
 * cover; do the same here, so 3D gets a selection frame and nothing else.
 */
@Composable
private fun Modifier.coverFrame(selected: Boolean, selectedWidth: Dp): Modifier {
    val idle = !CoverArtStyle.use3d.value
    return when {
        selected -> this.selectionOutline(true, RoundedCornerShape(12.dp), selectedWidth)
        idle -> this.border(1.dp, MaterialTheme.colorScheme.outline.copy(alpha = 0.42f), RoundedCornerShape(12.dp))
        else -> this
    }
}

@Composable
private fun GameCover(
    game: GameInfo,
    modifier: Modifier = Modifier,
    cornerRadius: Dp = 12.dp,
    // Fit (not Crop) everywhere so the whole box art shows — Crop trims the top of
    // covers whose source art is a touch taller than the 0.7 slot.
    contentScale: ContentScale = ContentScale.Fit,
    placeholderText: Boolean = true,
) {
    val context = LocalContext.current
    // Read the 3D-cover flag explicitly (not just via game.coverUrl, which is
    // skipped when a custom cover wins) so EVERY card — the Recently Played shelf
    // included — is subscribed and re-resolves when the toolbar toggle flips.
    val use3d = CoverArtStyle.use3d.value
    val customCoverMap = LocalCustomCoverMap.current
    val custom = remember(game.uri, customCoverMap) { CustomCovers.matchIn(customCoverMap, game) }
    val models = remember(custom, game, use3d, CustomCovers.version.value) {
        listOfNotNull(custom) + game.coverUrls
    }
    FallbackCoverImage(
        models = models,
        contentDescription = game.displayTitle(EnglishTitles.enabled.value),
        modifier = modifier.clip(RoundedCornerShape(cornerRadius)),
        contentScale = contentScale,
    ) {
        CoverPlaceholder(game.displayTitle(EnglishTitles.enabled.value), game.serial, showText = placeholderText)
    }
}

@Composable
private fun CoverPlaceholder(title: String, serial: String?, showText: Boolean) {
    Box(
        Modifier.fillMaxSize().background(
            Brush.linearGradient(
                listOf(MaterialTheme.colorScheme.primaryContainer, MaterialTheme.colorScheme.surfaceVariant),
            ),
        ),
        contentAlignment = Alignment.Center,
    ) {
        if (showText) {
            Column(Modifier.padding(8.dp), horizontalAlignment = Alignment.CenterHorizontally) {
                Text(
                    text = title,
                    style = MaterialTheme.typography.titleSmall,
                    fontWeight = FontWeight.Bold,
                    color = MaterialTheme.colorScheme.onPrimaryContainer,
                    textAlign = TextAlign.Center,
                    maxLines = 4,
                    overflow = TextOverflow.Ellipsis,
                )
                if (!serial.isNullOrBlank()) {
                    Text(
                        text = serial,
                        style = MaterialTheme.typography.labelSmall,
                        color = MaterialTheme.colorScheme.onPrimaryContainer.copy(alpha = 0.66f),
                        textAlign = TextAlign.Center,
                        maxLines = 1,
                        overflow = TextOverflow.Ellipsis,
                    )
                }
            }
        }
    }
}

/** Vertical focus zones of the library screen, stacked top→bottom. */
enum class HomeZone { Toolbar, Search, Collections, RecentsHeader, Recents, Grid }

object HomeInputController {
    private var owner: HomeViewModel? = null
    private var openMenu: (() -> Unit)? = null
    private var columns = 3
    val scrollVelocity = mutableFloatStateOf(0f)

    // Separate vertical zones stacked above the cover grid. The grid itself is
    // data-driven (owner.selectedIndex); the Recently-Played row and the top toolbar
    // (refresh / sort / layout / 2D-3D / background + the leading menu button) sit
    // above it and can't share that index space, so each is its own zone. From the
    // grid's top row, Up steps into Recents (if shown), then its Clear All header,
    // Collections, Search and Toolbar; Down walks back down. Left/Right move within the focused row. A fires
    // the highlighted item. HomeScreen reads `zone` + `toolbarIndex` / `recentIndex`
    // to draw the highlight and registers the toolbar actions + recents launcher.
    val zone = mutableStateOf(HomeZone.Grid)
    val toolbarIndex = mutableIntStateOf(0)
    val collectionIndex = mutableIntStateOf(0)
    val recentIndex = mutableIntStateOf(0)
    private var toolbarActions: List<() -> Unit> = emptyList()
    private var collectionActions: List<() -> Unit> = emptyList()
    private var collectionEditActions: List<(() -> Unit)?> = emptyList()
    private var collectionKeys: List<String> = emptyList()
    val movingTile = mutableStateOf<String?>(null)
    private val confirmPress = com.armsx2.ui.emulation.MenuConfirmPress()
    private val confirmHandler = android.os.Handler(android.os.Looper.getMainLooper())
    private var confirmHold: Runnable? = null
    private var pendingTile: String? = null
    private var collapseCollection: (() -> Boolean)? = null
    private var recentCount = 0
    private var clearRecentsAction: (() -> Unit)? = null
    private var recentLauncher: ((Int) -> Unit)? = null
    private var searchAvailable = false
    private var searchConfirm: (() -> Unit)? = null
    private var toolbarAtBottom = false

    /** True once the user has actually moved the selector with the controller. The
     *  grid camera-follow is gated on this so a cold app-open rests at the TOP (toolbar
     *  / search / recently-played) instead of the follow snapping the grid to the
     *  initially-selected cover and hiding the whole header. Reset in bind() so each
     *  time the library (re)opens it starts at the top again. */
    var userNavigated = false

    fun bind(viewModel: HomeViewModel, onOpenMenu: () -> Unit) {
        finishTileMove()
        confirmPress.reset()
        owner = viewModel
        openMenu = onOpenMenu
        userNavigated = false
    }

    fun unbind(viewModel: HomeViewModel) {
        if (owner === viewModel) {
            finishTileMove()
            confirmPress.reset()
            owner = null
            openMenu = null
            clearRecentsAction = null
            collectionActions = emptyList()
            collectionEditActions = emptyList()
            collectionKeys = emptyList()
            collapseCollection = null
            collectionIndex.intValue = 0
            scrollVelocity.floatValue = 0f
            zone.value = HomeZone.Grid
        }
    }

    fun setColumnCount(value: Int) { columns = value.coerceAtLeast(1) }
    fun active(): Boolean = owner != null

    /** HomeScreen registers the toolbar button actions here, in visual left→right
     *  order (leading menu button first). */
    fun setToolbarActions(actions: List<() -> Unit>) { toolbarActions = actions }

    fun setCollectionActions(actions: List<() -> Unit>, editActions: List<(() -> Unit)?> = emptyList(),
        keys: List<String> = emptyList(), collapse: (() -> Boolean)? = null) {
        collectionActions = actions
        collectionEditActions = editActions
        collectionKeys = keys
        collapseCollection = collapse
        movingTile.value?.let { id ->
            val index = keys.indexOf(id)
            if (index < 0) finishTileMove() else collectionIndex.intValue = index
        }
        collectionIndex.intValue = collectionIndex.intValue.coerceIn(0, actions.lastIndex.coerceAtLeast(0))
        if (actions.isEmpty() && zone.value == HomeZone.Collections) zone.value = HomeZone.Grid
    }

    private fun cancelTileConfirm() {
        confirmHold?.let(confirmHandler::removeCallbacks)
        confirmHold = null
        pendingTile = null
        confirmPress.cancel()
    }

    fun finishTileMove() {
        cancelTileConfirm()
        confirmPress.reset()
        movingTile.value = null
    }

    fun beginTileMove(id: String) {
        if (id !in com.armsx2.data.library.GameCollections.homeTileKeys()) return
        val index = collectionKeys.indexOf(id)
        if (index < 0) return
        cancelTileConfirm()
        movingTile.value = id
        collectionIndex.intValue = index
        zone.value = HomeZone.Collections
    }

    fun moveTile(direction: Int) {
        val id = movingTile.value ?: return
        com.armsx2.data.library.GameCollections.moveHomeTile(id, direction)
    }

    /** Folder taps open on release; a hold claims the press for arranging Home. */
    fun confirmKey(event: android.view.KeyEvent): Boolean {
        if (event.action == android.view.KeyEvent.ACTION_UP && confirmPress.matches(event.keyCode)) {
            confirmHold?.let(confirmHandler::removeCallbacks)
            confirmHold = null
            val id = pendingTile
            pendingTile = null
            if (confirmPress.up(event.keyCode, event.isCanceled)) {
                if (movingTile.value != null) finishTileMove()
                else if (zone.value == HomeZone.Collections && collectionKeys.getOrNull(collectionIndex.intValue) == id) confirm()
            }
            return true
        }
        val id = collectionKeys.getOrNull(collectionIndex.intValue)
        if (zone.value == HomeZone.Collections && (movingTile.value != null ||
                id in com.armsx2.data.library.GameCollections.homeTileKeys())) {
            if (event.action == android.view.KeyEvent.ACTION_DOWN && event.repeatCount == 0 && confirmPress.down(event.keyCode)) {
                pendingTile = id
                if (movingTile.value == null && id != null) {
                    val hold = Runnable {
                        confirmHold = null
                        if (active() && zone.value == HomeZone.Collections && pendingTile == id &&
                            collectionKeys.getOrNull(collectionIndex.intValue) == id &&
                            !com.armsx2.navigation.UiNavigator.drawerOpen.value && !LibraryKeyboard.visible.value &&
                            com.armsx2.ui.settings.SettingsControllerNav.activeLayer.value == null) beginTileMove(id)
                    }
                    confirmHold = hold
                    confirmHandler.postDelayed(hold, android.view.ViewConfiguration.getLongPressTimeout().toLong())
                }
            }
            return true
        }
        return event.action != android.view.KeyEvent.ACTION_DOWN || event.repeatCount != 0 || confirm()
    }

    fun setClearRecentsAction(action: () -> Unit) { clearRecentsAction = action }

    /** HomeScreen registers the currently-shown Recently-Played games (0 when the
     *  shelf is hidden, e.g. while searching). */
    fun setRecents(count: Int, launcher: (Int) -> Unit) {
        recentCount = count
        recentLauncher = launcher
        if (recentIndex.intValue >= count) recentIndex.intValue = (count - 1).coerceAtLeast(0)
        if (count == 0 && (zone.value == HomeZone.Recents || zone.value == HomeZone.RecentsHeader)) zone.value = HomeZone.Grid
    }

    /** HomeScreen registers whether the search field is shown (only once the library
     *  has loaded) and how to focus it — A on the Search zone opens the keyboard. */
    fun setSearchAction(available: Boolean, action: () -> Unit) {
        searchAvailable = available
        searchConfirm = action
        if (!available && zone.value == HomeZone.Search) zone.value = HomeZone.Grid
    }

    /** HomeScreen registers where the view toolbar is drawn (App setting). At the
     *  bottom it's reached by pressing Down off the grid's last row instead of Up. */
    fun setToolbarAtBottom(value: Boolean) { toolbarAtBottom = value }

    /** The chrome zone directly above the grid, honoring which chrome is shown and
     *  whether the toolbar is at the top. Order top→bottom (toolbar-top): Toolbar,
     *  Search, Collections, Recents, Grid. When the toolbar is at the bottom it isn't above. */
    private fun zoneAboveGrid(): HomeZone = when {
        recentCount > 0 -> HomeZone.Recents
        collectionActions.isNotEmpty() -> HomeZone.Collections
        searchAvailable -> HomeZone.Search
        !toolbarAtBottom && toolbarActions.isNotEmpty() -> HomeZone.Toolbar
        else -> HomeZone.Grid
    }

    fun move(dx: Int, dy: Int): Boolean {
        val viewModel = owner ?: return false
        cancelTileConfirm()
        if (movingTile.value != null) {
            if (dx != 0) moveTile(dx)
            return true
        }
        userNavigated = true
        // Snapshot the highlight so we blip the nav sound only when it actually moves (not when a
        // press runs into an edge).
        val beforeZone = zone.value
        val beforeToolbar = toolbarIndex.intValue
        val beforeCollection = collectionIndex.intValue
        val beforeRecent = recentIndex.intValue
        val beforeSel = viewModel.state.value.selectedIndex
        when (zone.value) {
            HomeZone.Toolbar -> when {
                // Toolbar at top: Down descends into the chrome/grid. At bottom: Up
                // returns to the grid.
                !toolbarAtBottom && dy > 0 -> zone.value = when {
                    searchAvailable -> HomeZone.Search
                    collectionActions.isNotEmpty() -> HomeZone.Collections
                    recentCount > 0 -> HomeZone.RecentsHeader
                    else -> HomeZone.Grid
                }
                toolbarAtBottom && dy < 0 -> zone.value = HomeZone.Grid
                dx != 0 && toolbarActions.isNotEmpty() ->
                    toolbarIndex.intValue = (toolbarIndex.intValue + dx).coerceIn(0, toolbarActions.lastIndex)
            }
            HomeZone.Search -> when {
                dy < 0 -> if (!toolbarAtBottom && toolbarActions.isNotEmpty()) zone.value = HomeZone.Toolbar
                dy > 0 && collectionActions.isNotEmpty() -> zone.value = HomeZone.Collections
                dy > 0 -> zone.value = if (recentCount > 0) HomeZone.RecentsHeader else HomeZone.Grid
            }
            HomeZone.Collections -> when {
                dy < 0 -> zone.value = when {
                    searchAvailable -> HomeZone.Search
                    !toolbarAtBottom && toolbarActions.isNotEmpty() -> HomeZone.Toolbar
                    else -> HomeZone.Collections
                }
                dy > 0 -> zone.value = if (recentCount > 0) HomeZone.RecentsHeader else HomeZone.Grid
                dx != 0 && collectionActions.isNotEmpty() ->
                    collectionIndex.intValue = (collectionIndex.intValue + dx).coerceIn(0, collectionActions.lastIndex)
            }
            HomeZone.RecentsHeader -> when {
                dy > 0 -> zone.value = HomeZone.Recents
                dy < 0 -> zone.value = when {
                    collectionActions.isNotEmpty() -> HomeZone.Collections
                    searchAvailable -> HomeZone.Search
                    !toolbarAtBottom && toolbarActions.isNotEmpty() -> HomeZone.Toolbar
                    else -> HomeZone.RecentsHeader
                }
            }
            HomeZone.Recents -> when {
                dy < 0 -> zone.value = HomeZone.RecentsHeader
                dy > 0 -> zone.value = HomeZone.Grid
                dx != 0 && recentCount > 0 ->
                    recentIndex.intValue = (recentIndex.intValue + dx).coerceIn(0, recentCount - 1)
            }
            HomeZone.Grid -> when {
                dx != 0 -> viewModel.moveSelection(dx)
                // Up: try to climb a row; if the selection didn't move we're on the top
                // row → step into the chrome above. This "move-then-check" is robust to
                // the exact per-row count (shelf/grid), unlike a selectedIndex<columns
                // guess — which is why Recently Played was being skipped in shelf view.
                dy < 0 -> {
                    val before = viewModel.state.value.selectedIndex
                    viewModel.moveSelection(-columns)
                    if (viewModel.state.value.selectedIndex == before && zoneAboveGrid() != HomeZone.Grid) {
                        zone.value = zoneAboveGrid()
                    }
                }
                // Down: climb a row; if it didn't move we're on the last row → step into
                // the bottom toolbar (only when the toolbar is drawn there).
                dy > 0 -> {
                    val before = viewModel.state.value.selectedIndex
                    viewModel.moveSelection(columns)
                    if (viewModel.state.value.selectedIndex == before && toolbarAtBottom && toolbarActions.isNotEmpty()) {
                        zone.value = HomeZone.Toolbar
                    }
                }
            }
        }
        if (zone.value != beforeZone || toolbarIndex.intValue != beforeToolbar ||
            collectionIndex.intValue != beforeCollection ||
            recentIndex.intValue != beforeRecent || viewModel.state.value.selectedIndex != beforeSel
        ) com.armsx2.MenuSfx.play(com.armsx2.MenuSfx.Event.NAV)
        return true
    }

    fun confirm(): Boolean {
        val viewModel = owner ?: return false
        com.armsx2.MenuSfx.play(com.armsx2.MenuSfx.Event.SELECT)
        when (zone.value) {
            HomeZone.Toolbar -> toolbarActions.getOrNull(toolbarIndex.intValue)?.invoke()
            HomeZone.Search -> searchConfirm?.invoke()
            HomeZone.Collections -> collectionActions.getOrNull(collectionIndex.intValue)?.invoke()
            HomeZone.RecentsHeader -> clearRecentsAction?.invoke()
            HomeZone.Recents -> recentLauncher?.invoke(recentIndex.intValue)
            HomeZone.Grid -> {
                val game = viewModel.selectedGame() ?: return false
                viewModel.launch(game)
            }
        }
        return true
    }

    fun openSelectedSettings(): Boolean {
        finishTileMove()
        if (zone.value == HomeZone.Collections) {
            val edit = collectionEditActions.getOrNull(collectionIndex.intValue) ?: return false
            edit()
            return true
        }
        val game = owner?.selectedGame() ?: return false
        com.armsx2.navigation.UiNavigator.navigate(com.armsx2.navigation.AppRoute.Settings(game = game))
        return true
    }

    fun scroll(velocity: Float): Boolean {
        if (owner == null) return false
        scrollVelocity.floatValue = if (abs(velocity) > 0.08f) velocity.coerceIn(-1f, 1f) else 0f
        return true
    }

    /** Leave the selected collection without changing its saved contents. */
    fun returnToLibrary(): Boolean {
        val viewModel = owner ?: return false
        if (com.armsx2.data.library.GameCollections.selectedId.value == null) return false
        com.armsx2.data.library.GameCollections.select(null)
        viewModel.collectionsChanged()
        zone.value = HomeZone.Grid
        userNavigated = false
        return true
    }

    fun back(): Boolean {
        com.armsx2.MenuSfx.play(com.armsx2.MenuSfx.Event.BACK)
        if (movingTile.value != null) { finishTileMove(); return true }
        cancelTileConfirm()
        if (collapseCollection?.invoke() == true) {
            zone.value = HomeZone.Collections
            return true
        }
        if (returnToLibrary()) return true
        // B in the Recents / Toolbar zone drops back to the grid; on the grid it
        // opens the nav drawer.
        if (zone.value != HomeZone.Grid) {
            zone.value = HomeZone.Grid
            return true
        }
        openMenu?.invoke() ?: return false
        return true
    }

    /** Cycle the library layout Grid → List → Shelf (bound to R1). Gives the
     *  controller direct access to all three view modes without needing to reach
     *  the touch-only toolbar toggle. */
    fun cycleLayout(): Boolean {
        val viewModel = owner ?: return false
        viewModel.toggleLayout()
        return true
    }

    /** Cycle the library sort order (bound to L1). */
    fun cycleSort(): Boolean {
        val viewModel = owner ?: return false
        val entries = HomeSort.entries
        val next = entries[(viewModel.state.value.sort.ordinal + 1) % entries.size]
        viewModel.setSort(next)
        return true
    }
}

// -- vivi's shelf library layout — covers standing on vivi's plank PNGs (frosted
//    for All Games, navy for Now Playing). Each cover sits seated on the plank's
//    top face (back-of-centre, like a book on a shelf) with shelf surface visible
//    in front of it, and a faint mirror reflection on that surface. --

@Composable
private fun GameShelf(
    games: List<GameInfo>,
    shelfRes: Int,
    coverWidth: Dp,
    scroll: Boolean,
    onLaunch: (GameInfo) -> Unit,
    onDetails: (GameInfo) -> Unit = {},
    modifier: Modifier = Modifier,
    slotsPerRow: Int = games.size,
    // Controller selection highlight: the global visibleGames index that's selected,
    // and this shelf row's first global index. -1 = nothing selected on this shelf.
    selectedIndex: Int = -1,
    startIndex: Int = 0,
) {
    val coverHeight = coverWidth / 0.7f
    // Slimmer plank to match bagas's slimmer frosted-shelf PNG (2903×200).
    val plankHeight = 84.dp
    // How far below the plank's back (top) edge the cover base sits — small, so the
    // cover rests toward the BACK of the top face with plenty of shelf in front of
    // it (not perched on the front edge). Scaled with the slimmer plank.
    val surfaceInset = 16.dp
    val reflectionHeight = coverHeight * 0.18f
    // Covers + reflection are top-anchored; the box is tall enough that the cover
    // base lands on the top face and the reflection lays over the shelf in front.
    // "Name on grid" also labels shelf covers (previously only the flat grid honoured it) — reserve
    // a line under the reflection for the title so it isn't clipped by the plank.
    val nameHeight = if (GridLabels.show.value) 18.dp else 0.dp
    val rowHeight = coverHeight + reflectionHeight + nameHeight
    Box(modifier.fillMaxWidth().height(coverHeight + plankHeight - surfaceInset + nameHeight)) {
        Image(
            painter = painterResource(shelfRes),
            contentDescription = null,
            // The shelf PNG carries a ~1.1% transparent/faded margin on each side, so
            // when stretched full-width its *visible* plank edge sits ~11dp inset while
            // the first/last covers reach the screen edge — making them overhang the
            // shelf's faded end. Scale the plank out ~5% horizontally so its solid
            // surface bleeds to (past) the screen edges and the covers sit on it.
            modifier = Modifier.align(Alignment.BottomCenter).fillMaxWidth().height(plankHeight)
                .graphicsLayer { scaleX = 1.05f },
            contentScale = ContentScale.FillBounds,
        )
        if (scroll) {
            // Controller selection must drive the same blue highlight the non-scroll
            // path draws, and keep the selected cover on-screen — without this the
            // Recently-Played shelf (scroll = true) showed NO highlight, so it looked
            // like the controller couldn't select it.
            val rowState = rememberLazyListState()
            LaunchedEffect(selectedIndex) {
                val local = selectedIndex - startIndex
                if (local in games.indices) rowState.animateScrollToItem(local)
            }
            LazyRow(
                state = rowState,
                modifier = Modifier.align(Alignment.TopStart).fillMaxWidth().height(rowHeight),
                // 12dp start so the first cover lines up with the section header.
                contentPadding = PaddingValues(horizontal = 12.dp),
                horizontalArrangement = Arrangement.spacedBy(16.dp),
            ) {
                itemsIndexed(games, key = { _, g -> "shelfcard_${shelfRes}_${g.uri}" }) { index, game ->
                    ShelfGameCard(
                        game, coverWidth, reflectionHeight,
                        selected = startIndex + index == selectedIndex,
                        onLaunch = onLaunch,
                        onDetails = onDetails,
                    )
                }
            }
        } else {
            // Covers laid left-to-right at their full-row positions. SpaceBetween
            // pins the first cover to the left padding (aligned with the section
            // header) and spreads the row across the shelf width. A short trailing
            // row is padded with invisible spacers so its covers stay packed on the
            // left in sequence instead of sprawling across the whole shelf.
            Row(
                modifier = Modifier.align(Alignment.TopStart).fillMaxWidth().height(rowHeight).padding(horizontal = 12.dp),
                horizontalArrangement = Arrangement.SpaceBetween,
            ) {
                games.forEachIndexed { i, game ->
                    ShelfGameCard(game, coverWidth, reflectionHeight, selected = startIndex + i == selectedIndex, onLaunch = onLaunch, onDetails = onDetails)
                }
                repeat((slotsPerRow - games.size).coerceAtLeast(0)) {
                    Spacer(Modifier.width(coverWidth))
                }
            }
        }
    }
}

@Composable
private fun ShelfGameCard(game: GameInfo, width: Dp, reflectionHeight: Dp, selected: Boolean = false, onLaunch: (GameInfo) -> Unit, onDetails: (GameInfo) -> Unit = {}) {
    // Long-press opens the game context menu (per-game settings, hide, etc.) — same as the grid /
    // list / recents cards. Without this the shelf layout had no way to reach per-game settings.
    Column(modifier = Modifier.width(width).combinedClickable(onClick = { onLaunch(game) }, onLongClick = { onDetails(game) })) {
        // Square corners in shelf view — rounding fought the 3D box-art edges. The
        // grid/cover view keeps rounded corners (GameCover's 12.dp default).
        // ContentScale.Fit shows the WHOLE cover — Crop was trimming the top off the
        // large standing covers.
        GameCover(
            game,
            Modifier.fillMaxWidth().aspectRatio(0.7f)
                .then(
                    if (selected)
                        Modifier.border(2.5.dp, Color(0xFF3DA5FF), RoundedCornerShape(4.dp))
                    else Modifier,
                ),
            cornerRadius = 0.dp,
            contentScale = ContentScale.Fit,
        )
        // A faint mirror of the cover on the shelf surface just in front of it.
        // clipToBounds keeps it to reflectionHeight — without it the full flipped
        // cover renders and bleeds down onto the row below.
        Box(Modifier.fillMaxWidth().height(reflectionHeight).clipToBounds()) {
            GameCover(
                game,
                Modifier.fillMaxWidth().aspectRatio(0.7f)
                    .graphicsLayer { scaleY = -1f; alpha = 0.18f },
                cornerRadius = 0.dp,
                contentScale = ContentScale.Fit,
                placeholderText = false,
            )
            // Fade the reflection out toward the front of the shelf.
            Box(Modifier.matchParentSize().background(Brush.verticalGradient(listOf(Color.Transparent, Color(0x55000000)))))
        }
        // Title under the cover when "Name on grid" is on — shelf covers honour it too now.
        if (GridLabels.show.value) {
            Text(
                game.displayTitle(EnglishTitles.enabled.value),
                style = MaterialTheme.typography.labelSmall,
                maxLines = 1,
                overflow = TextOverflow.Ellipsis,
                textAlign = TextAlign.Center,
                modifier = Modifier.fillMaxWidth().padding(horizontal = 2.dp),
            )
        }
    }
}
