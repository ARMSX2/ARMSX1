package com.armsx2.ui.emulation

import androidx.activity.compose.BackHandler
import androidx.compose.animation.AnimatedVisibility
import androidx.compose.animation.core.EaseIn
import androidx.compose.animation.core.EaseOut
import androidx.compose.animation.core.tween
import androidx.compose.animation.fadeIn
import androidx.compose.animation.fadeOut
import androidx.compose.animation.slideInHorizontally
import androidx.compose.animation.slideOutHorizontally
import androidx.compose.foundation.BorderStroke
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ColumnScope
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.WindowInsets
import androidx.compose.foundation.layout.WindowInsetsSides
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.only
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.safeDrawing
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.layout.windowInsetsPadding
import androidx.compose.foundation.relocation.BringIntoViewRequester
import androidx.compose.foundation.relocation.bringIntoViewRequester
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.ScrollState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.layout.layout
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.lifecycle.viewmodel.compose.viewModel
import coil.compose.AsyncImage
import com.armsx2.i18n.str
import com.armsx2.runtime.MainActivityRuntime
import com.armsx2.ui.InGameOverlay
import com.armsx2.ui.achievements.AchievementItem
import com.armsx2.ui.common.GameCoverArt
import com.armsx2.ui.settings.controllerFocusable
import com.armsx2.ui.touch.TouchControls
import com.armsx2.ui.theme.Danger
import com.armsx2.ui.common.StatusChip
import com.armsx2.ui.theme.Success
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch

@Composable
fun EmulationMenuScreen(viewModel: EmulationMenuViewModel = viewModel()) {
    val state = viewModel.state.value
    val scope = rememberCoroutineScope()
    var shown by remember { mutableStateOf(false) }
    var dismissing by remember { mutableStateOf(false) }
    var friendsOpen by remember { mutableStateOf(false) }
    val closeMenu: () -> Unit = remember(viewModel, scope) {
        {
            if (!dismissing) {
                dismissing = true
                shown = false
                // ★ Dispatchers.Main, NOT the composition's own dispatcher. rememberCoroutineScope
                // inherits the composition context, which on Android is AndroidUiDispatcher — it
                // dispatches continuations on CHOREOGRAPHER FRAME CALLBACKS. We have just set
                // shown = false, so once the exit animation settles Compose has nothing left to
                // invalidate, no frame is scheduled, and the continuation after this delay is
                // never dispatched: the VM is simply never told to resume. The game sits paused
                // with the OSD reading "FPS: N/A" until something incidentally causes a frame —
                // which is exactly why tapping the on-screen controls "speeds up" the recovery
                // (touch input schedules a frame) and why waiting also eventually works.
                // Dispatchers.Main is a plain main-looper Handler dispatcher with no frame
                // dependency, so the resume fires on time whether or not anything is drawing.
                scope.launch(Dispatchers.Main) {
                    delay(220)
                    viewModel.dismissHandler = null
                    viewModel.resumeImmediately()
                }
            }
        }
    }

    DisposableEffect(viewModel, closeMenu) {
        viewModel.dismissHandler = closeMenu
        EmulationMenuInputController.bind(viewModel)
        onDispose {
            viewModel.dismissHandler = null
            EmulationMenuInputController.unbind(viewModel)
        }
    }
    LaunchedEffect(Unit) { shown = true }

    // Hand pad input to the Friends panel while it is open, and give it back on close.
    //
    // The nav registry is shared between the menu and the panel, so ownership has to be explicit:
    // the selection is cleared on both edges, because a selection left pointing at a control on
    // the other side of the transition highlights something the user cannot see.
    DisposableEffect(friendsOpen) {
        if (friendsOpen) {
            EmulationMenuInputController.overlayDismiss = { friendsOpen = false }
            com.armsx2.ui.settings.SettingsControllerNav.clearSelection()
        }
        onDispose {
            EmulationMenuInputController.overlayDismiss = null
            com.armsx2.ui.settings.SettingsControllerNav.clearSelection()
        }
    }
    // Highlight the panel's first control once it has actually composed. Selecting in the same
    // frame the panel opens would find an empty registry — controllerFocusable only registers
    // items that exist, and the panel's do not until AnimatedVisibility has run.
    LaunchedEffect(friendsOpen) {
        if (friendsOpen) {
            delay(260)
            if (friendsOpen) com.armsx2.ui.settings.SettingsControllerNav.move(1)
        }
    }
    // Back closes the friends overlay first when it is up. Without this, opening Friends and
    // pressing Back would dismiss the entire pause menu and resume the game, which is not what
    // anyone means by "go back" from a panel sitting on top of another panel.
    BackHandler(onBack = { if (friendsOpen) friendsOpen = false else closeMenu() })

    state.pendingHardcore?.let { enabling ->
        androidx.compose.runtime.DisposableEffect(Unit) {
            com.armsx2.MenuSfx.play(com.armsx2.MenuSfx.Event.POPUP_OPEN)
            onDispose { com.armsx2.MenuSfx.play(com.armsx2.MenuSfx.Event.POPUP_CLOSE) }
        }
        AlertDialog(
            onDismissRequest = viewModel::cancelToggleHardcore,
            title = { Text(str(if (enabling) "ra.hardcore.enable.title" else "ra.hardcore.disable.title")) },
            text = { Text(str(if (enabling) "ra.hardcore.enable.body" else "ra.hardcore.disable.body")) },
            confirmButton = {
                TextButton(onClick = viewModel::confirmToggleHardcore) {
                    Text(str(if (enabling) "ra.hardcore.enable.confirm" else "ra.hardcore.disable.confirm"))
                }
            },
            dismissButton = { TextButton(onClick = viewModel::cancelToggleHardcore) { Text(str("action.cancel")) } },
        )
    }

    BoxWithConstraints(Modifier.fillMaxSize()) {
        val compact = maxWidth < 700.dp
        AnimatedVisibility(
            visible = shown,
            enter = fadeIn(tween(190, easing = EaseOut)),
            exit = fadeOut(tween(190, easing = EaseIn)),
        ) {
            Box(
                Modifier
                    .fillMaxSize()
                    .background(Color.Black.copy(alpha = 0.62f))
                    .clickable(onClick = closeMenu),
            )
        }
        AnimatedVisibility(
            visible = shown,
            enter = slideInHorizontally(tween(320, easing = EaseOut)) { it },
            exit = slideOutHorizontally(tween(220, easing = EaseIn)) { it },
            modifier = Modifier.align(Alignment.CenterEnd),
        ) {
            if (compact) {
                Surface(
                    modifier = Modifier
                        .fillMaxHeight()
                        .fillMaxWidth(0.96f)
                        .windowInsetsPadding(WindowInsets.safeDrawing.only(WindowInsetsSides.Vertical)),
                    shape = RoundedCornerShape(topStart = 28.dp, bottomStart = 28.dp),
                    color = MaterialTheme.colorScheme.surface,
                    border = BorderStroke(1.dp, MaterialTheme.colorScheme.outline.copy(alpha = 0.5f)),
                    shadowElevation = 22.dp,
                ) {
                    MenuPage(
                        state = state,
                        viewModel = viewModel,
                        compact = true,
                        modifier = Modifier.fillMaxSize(),
                        onOpenFriends = { friendsOpen = true },
                    )
                }
            } else {
                Row(
                    modifier = Modifier
                        .fillMaxHeight()
                        .fillMaxWidth(0.64f)
                        .widthIn(max = 900.dp)
                        .windowInsetsPadding(WindowInsets.safeDrawing.only(WindowInsetsSides.Vertical))
                        .padding(top = 14.dp, end = 12.dp, bottom = 14.dp),
                    horizontalArrangement = Arrangement.spacedBy(10.dp),
                ) {
                    Surface(
                        modifier = Modifier.weight(1f).fillMaxHeight(),
                        shape = RoundedCornerShape(28.dp),
                        color = MaterialTheme.colorScheme.surface,
                        border = BorderStroke(1.dp, MaterialTheme.colorScheme.outline.copy(alpha = 0.5f)),
                        shadowElevation = 22.dp,
                    ) {
                        MenuPage(
                            state = state,
                            viewModel = viewModel,
                            compact = false,
                            modifier = Modifier.fillMaxSize(),
                            onOpenFriends = { friendsOpen = true },
                        )
                    }
                    Surface(
                        modifier = Modifier.width(76.dp).fillMaxHeight(),
                        shape = RoundedCornerShape(28.dp),
                        color = MaterialTheme.colorScheme.surfaceVariant.copy(alpha = 0.94f),
                        border = BorderStroke(1.dp, MaterialTheme.colorScheme.outline.copy(alpha = 0.5f)),
                        shadowElevation = 18.dp,
                    ) {
                        MenuRail(state.tab, viewModel::selectTab)
                    }
                }
            }
        }

        // Friends, as its own panel over the menu.
        //
        // Composed here rather than as an AlertDialog on purpose: a Dialog gets its own focused
        // window, and a focused window swallows gamepad keys before our input plumbing ever sees
        // them — the pause menu would stop responding to the pad the moment this opened.
        AnimatedVisibility(
            visible = friendsOpen,
            enter = fadeIn(tween(160, easing = EaseOut)),
            exit = fadeOut(tween(140, easing = EaseIn)),
        ) {
            Box(
                Modifier
                    .fillMaxSize()
                    .background(Color.Black.copy(alpha = 0.72f))
                    .clickable { friendsOpen = false },
            )
        }
        AnimatedVisibility(
            visible = friendsOpen,
            enter = fadeIn(tween(190, easing = EaseOut)),
            exit = fadeOut(tween(150, easing = EaseIn)),
            modifier = Modifier.align(Alignment.Center),
        ) {
            Surface(
                modifier = Modifier
                    .fillMaxWidth(if (compact) 0.94f else 0.6f)
                    .widthIn(max = 620.dp)
                    .fillMaxHeight(0.9f)
                    .windowInsetsPadding(WindowInsets.safeDrawing.only(WindowInsetsSides.Vertical)),
                shape = RoundedCornerShape(24.dp),
                color = MaterialTheme.colorScheme.surface,
                border = BorderStroke(1.dp, MaterialTheme.colorScheme.outline.copy(alpha = 0.5f)),
                shadowElevation = 24.dp,
            ) {
                Column(Modifier.fillMaxSize().verticalScroll(rememberScrollState())) {
                    Row(
                        Modifier.fillMaxWidth().padding(start = 16.dp, end = 10.dp, top = 14.dp),
                        verticalAlignment = Alignment.CenterVertically,
                    ) {
                        Text(
                            str("friends.title"),
                            style = MaterialTheme.typography.titleMedium,
                            fontWeight = FontWeight.Bold,
                        )
                        // Between the title and Close: whose Discord this is.
                        com.armsx2.ui.friends.SelfChip(
                            Modifier.weight(1f).padding(horizontal = 12.dp),
                        )
                        TextButton(
                            onClick = { friendsOpen = false },
                            modifier = Modifier.controllerFocusable(
                                "menu.friends.close",
                                onConfirm = { friendsOpen = false },
                            ),
                        ) { Text(str("action.close")) }
                    }
                    com.armsx2.ui.friends.FriendsPanel(Modifier.padding(horizontal = 12.dp))
                }
            }
        }
    }
}

@Composable
private fun MenuPage(
    state: EmulationMenuUiState,
    viewModel: EmulationMenuViewModel,
    compact: Boolean,
    modifier: Modifier,
    onOpenFriends: () -> Unit,
) {
    val tabScrollStates = remember {
        EmulationMenuTab.entries.associateWith {
            ScrollState(initial = InGameOverlay.menuTabScroll[it.name] ?: 0)
        }
    }
    // Remember each tab's scroll offset when the menu closes so reopening a tab (especially
    // the long Fixes list) returns to where you were instead of snapping back to the top.
    androidx.compose.runtime.DisposableEffect(Unit) {
        onDispose {
            tabScrollStates.forEach { (tab, ss) -> InGameOverlay.menuTabScroll[tab.name] = ss.value }
        }
    }
    val scrollState = tabScrollStates.getValue(state.tab)
    // Provide the pane's scroll state to the settings widgets so the Fixes pane's
    // right-stick free-scroll (settingsScrollState / ControllerAutoScroll) drives the
    // pane the user is actually looking at. Per-control bring-into-view handles the
    // primary "keep selection on screen" via the nearest scrollable ancestor already.
    androidx.compose.runtime.CompositionLocalProvider(
        com.armsx2.ui.settings.LocalSettingsScrollState provides scrollState,
    ) {
        Column(
            modifier
                .verticalScroll(scrollState)
                .padding(bottom = 18.dp),
        ) {
            if (compact) CompactMenuTabs(state.tab, viewModel::selectTab)
            MenuHeader(compact, state.hardcore, state.richPresence, state.gameCRC, onOpenFriends)
            HorizontalDivider(
                modifier = Modifier.padding(horizontal = 8.dp),
                color = MaterialTheme.colorScheme.outline.copy(alpha = 0.34f),
            )
            Column(
                Modifier.fillMaxWidth().padding(horizontal = 8.dp, vertical = 12.dp),
                verticalArrangement = Arrangement.spacedBy(10.dp),
            ) {
                when (state.tab) {
                    EmulationMenuTab.Session -> SessionPane(state, viewModel)
                    EmulationMenuTab.Graphics -> GraphicsPane(state, viewModel)
                    // Was the PS2 Fixes tab (GameDB fixes / recompiler / upscaling hacks). The PS1
                    // core has no hack surface, so this slot now shows the same Advanced page the
                    // settings hub does — logging + paths, written to settings.toml. Its controls
                    // are SettingsControllerNav items, so the pause menu's content-pane nav still
                    // drives them for free.
                    EmulationMenuTab.Fixes -> com.armsx2.ui.settings.Ps1AdvancedTab()
                    EmulationMenuTab.Performance -> PerformancePane(state, viewModel)
                    EmulationMenuTab.Controls -> ControlsPane(state, viewModel)
                    EmulationMenuTab.Options -> OptionsPane(state, viewModel)
                    EmulationMenuTab.Achievements -> AchievementsPane(state, viewModel)
                }
            }
        }
    }
}

@Composable
private fun CompactMenuTabs(selected: EmulationMenuTab, onSelect: (EmulationMenuTab) -> Unit) {
    Row(
        Modifier
            .fillMaxWidth()
            .horizontalScroll(rememberScrollState())
            .padding(horizontal = 8.dp),
        horizontalArrangement = Arrangement.spacedBy(6.dp),
    ) {
        EmulationMenuTab.entries.forEach { tab ->
            MenuTab(tab, tab == selected, onSelect)
        }
    }
}

@Composable
private fun MenuRail(
    selected: EmulationMenuTab,
    onSelect: (EmulationMenuTab) -> Unit,
) {
    Column(
        Modifier
            .fillMaxHeight()
            .verticalScroll(rememberScrollState())
            .padding(horizontal = 8.dp, vertical = 10.dp),
        horizontalAlignment = Alignment.CenterHorizontally,
        // Centred, not top-aligned: the rail fills the full height, so with the tabs pinned
        // to the top the column left a block of dead space at the bottom once the duplicate
        // All Settings shortcut was removed from under them. Centring keeps the group
        // balanced regardless of how many tabs there are, and still scrolls if it ever
        // outgrows the rail.
        verticalArrangement = Arrangement.spacedBy(6.dp, Alignment.CenterVertically),
    ) {
        EmulationMenuTab.entries.forEach { tab ->
            MenuRailTab(tab, tab == selected, onSelect)
        }
    }
}

@Composable
private fun MenuRailTab(tab: EmulationMenuTab, active: Boolean, onSelect: (EmulationMenuTab) -> Unit) {
    val bring = remember { BringIntoViewRequester() }
    val label = str(tab.titleKey)
    LaunchedEffect(active) { if (active) runCatching { bring.bringIntoView() } }
    Surface(
        onClick = { onSelect(tab) },
        modifier = Modifier.size(56.dp).bringIntoViewRequester(bring).semantics { contentDescription = label },
        shape = RoundedCornerShape(18.dp),
        color = if (active) MaterialTheme.colorScheme.primaryContainer else MaterialTheme.colorScheme.surface.copy(alpha = 0.5f),
        border = BorderStroke(
            if (active) 2.dp else 1.dp,
            if (active) MaterialTheme.colorScheme.primary else MaterialTheme.colorScheme.outline.copy(alpha = 0.34f),
        ),
    ) {
        Box(contentAlignment = Alignment.Center) {
            Text(
                text = tabGlyph(tab),
                fontSize = 23.sp,
                fontWeight = FontWeight.Bold,
                color = if (active) MaterialTheme.colorScheme.primary else MaterialTheme.colorScheme.onSurfaceVariant,
            )
        }
    }
}

@Composable
private fun MenuTab(tab: EmulationMenuTab, active: Boolean, onSelect: (EmulationMenuTab) -> Unit) {
    // Keep the active tab scrolled into view so controller nav reaches tabs that fall off
    // the rail on short screens — e.g. the 7th "Achievements" (RA) tab on a Retroid Pocket
    // in landscape. Mirrors the settings-hub / library camera-follow. Resolves against the
    // nearest scrollable ancestor, so it works for both the vertical rail and the compact
    // horizontal strip.
    val bring = remember { BringIntoViewRequester() }
    LaunchedEffect(active) { if (active) runCatching { bring.bringIntoView() } }
    Surface(
        onClick = { onSelect(tab) },
        modifier = Modifier
            .widthIn(min = 132.dp, max = 210.dp)
            .padding(vertical = 3.dp)
            .bringIntoViewRequester(bring),
        shape = RoundedCornerShape(14.dp),
        color = if (active) MaterialTheme.colorScheme.primaryContainer
        else MaterialTheme.colorScheme.surfaceVariant.copy(alpha = 0.56f),
        border = BorderStroke(
            if (active) 1.5.dp else 1.dp,
            if (active) MaterialTheme.colorScheme.primary.copy(alpha = 0.72f)
            else MaterialTheme.colorScheme.outline.copy(alpha = 0.32f),
        ),
    ) {
        Row(
            modifier = Modifier.padding(horizontal = 11.dp, vertical = 9.dp),
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.spacedBy(8.dp),
        ) {
            Surface(
                modifier = Modifier.size(30.dp),
                shape = RoundedCornerShape(9.dp),
                color = if (active) MaterialTheme.colorScheme.primary.copy(alpha = 0.14f)
                else MaterialTheme.colorScheme.surface.copy(alpha = 0.72f),
            ) {
                Box(contentAlignment = Alignment.Center) {
                    Text(
                        text = tabGlyph(tab),
                        fontSize = 16.sp,
                        fontWeight = FontWeight.Bold,
                        color = if (active) MaterialTheme.colorScheme.primary else MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                }
            }
            Text(
                text = str(tab.titleKey),
                color = if (active) MaterialTheme.colorScheme.onPrimaryContainer else MaterialTheme.colorScheme.onSurfaceVariant,
                style = MaterialTheme.typography.labelLarge,
                fontWeight = if (active) FontWeight.Bold else FontWeight.Medium,
                maxLines = 2,
                overflow = TextOverflow.Ellipsis,
            )
        }
    }
}

// Rail tab icons. No monochrome Unicode exists for gamepad/wrench/trophy/display, so those
// use color emoji (the bundled NotoColorEmoji renders them); Session keeps its clean text
// glyph. Performance uses the high-voltage emoji so it reads as a yellow lightning bolt.
// Options carries the settings gear; the full-settings shortcut below the rail divider uses
// a distinct "open" glyph so there aren't two gears.
private fun tabGlyph(tab: EmulationMenuTab): String = when (tab) {
    EmulationMenuTab.Session -> "☰"
    EmulationMenuTab.Graphics -> "🖥️"
    EmulationMenuTab.Fixes -> "🔧"
    EmulationMenuTab.Performance -> "⚡"
    EmulationMenuTab.Controls -> "🎮"
    EmulationMenuTab.Options -> "⚙"
    EmulationMenuTab.Achievements -> "🏆"
}

@Composable
private fun MenuHeader(
    compact: Boolean,
    hardcore: Boolean,
    richPresence: String,
    gameCRC: String,
    onOpenFriends: () -> Unit,
) {
    val game = MainActivityRuntime.currentGame.value
    Row(
        Modifier.fillMaxWidth().padding(horizontal = if (compact) 12.dp else 16.dp, vertical = 12.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        if (game != null) {
            GameCoverArt(game, Modifier.width(if (compact) 38.dp else 44.dp).height(if (compact) 52.dp else 60.dp))
            Spacer(Modifier.width(11.dp))
        }
        Column(Modifier.weight(1f)) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Text(
                    game?.title ?: "PlayStation",
                    style = MaterialTheme.typography.titleMedium,
                    fontWeight = FontWeight.Bold,
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis,
                    color = MaterialTheme.colorScheme.onSurface,
                    modifier = Modifier.weight(1f, fill = false),
                )
                if (hardcore) {
                    Spacer(Modifier.width(8.dp))
                    HardcoreBadge()
                }
                // File-type chip after the HC badge (ISO / CHD / …), mirroring the library
                // list view so the pause/RA header shows the same at-a-glance file info.
                game?.let { g ->
                    Spacer(Modifier.width(6.dp))
                    com.armsx2.ui.common.StatusChip(g.extension.ifBlank { g.platform.key.uppercase() })
                }
            }
            // Serial and CRC together: a PNACH is named <SERIAL>_<CRC>.pnach, so the two values
            // needed to name one should not live on separate screens.
            //
            // The live VM CRC is preferred but cannot be relied on: for ISO boots the core hands
            // ELFLoadingOnCPUThread an empty path, so UpdateELFInfo takes its failure branch and
            // leaves s_current_crc at 0 — the emulog shows the loader computing the real CRC and
            // the VM then reporting 00000000. When that happens, identify the image instead, which
            // is the same path the Info tab and the library's long-press sheet already take.
            val resolvedCRC by androidx.compose.runtime.produceState(gameCRC, gameCRC, game?.uri) {
                value = gameCRC.ifBlank {
                    game?.uri?.let { com.armsx2.DiscIdentity.resolve(it, game.serial) }.orEmpty()
                }
            }
            val identity = buildList {
                game?.serial?.takeIf { it.isNotBlank() }?.let(::add)
                resolvedCRC.takeIf { it.isNotBlank() }?.let { add("CRC $it") }
            }.joinToString("  ·  ")
            if (identity.isNotBlank()) {
                Spacer(Modifier.height(2.dp))
                Text(
                    identity,
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }
            // RetroAchievements rich presence — the live "what you're doing" line
            // (e.g. "Pooh & Piglet are in a Scaring Contest"). Restored from the old UI.
            if (richPresence.isNotBlank()) {
                Spacer(Modifier.height(2.dp))
                Text(
                    richPresence,
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis,
                )
            }
        }

        // Clock + battery, same cluster as the library toolbar. Worth having here specifically:
        // this menu is what you open mid-session on a handheld, so it's exactly when you want to
        // know the time and how much charge is left. Not controllerFocusable — it's a readout.
        Spacer(Modifier.width(8.dp))
        com.armsx2.ui.common.LibraryStatusCluster(
            Modifier.align(Alignment.CenterVertically),
        )

        // Friends, in the header where it is always visible, with the online count on it. A build
        // without the SDK has nothing to show, so it does not take up header space there.
        if (com.armsx2.DiscordPresence.available()) {
            Spacer(Modifier.width(8.dp))
            Surface(
                onClick = onOpenFriends,
                modifier = Modifier.controllerFocusable(
                    "menu.friends",
                    RoundedCornerShape(14.dp),
                    onConfirm = onOpenFriends,
                ),
                shape = RoundedCornerShape(14.dp),
                color = MaterialTheme.colorScheme.surfaceVariant.copy(alpha = 0.7f),
                border = BorderStroke(1.dp, MaterialTheme.colorScheme.outline.copy(alpha = 0.5f)),
            ) {
                Box(Modifier.padding(horizontal = 11.dp, vertical = 9.dp)) {
                    com.armsx2.ui.friends.FriendsGlyphWithBadge(
                        color = MaterialTheme.colorScheme.onSurface,
                        glyphSize = 19.sp,
                    )
                }
            }
        }
    }
}

@Composable
private fun SessionPane(state: EmulationMenuUiState, viewModel: EmulationMenuViewModel) {
    ActionGrid(
        actions = listOf(
            MenuAction(str("action.resume"), str("action.play"), "▶", Success, viewModel::resume),
            MenuAction(
                str("action.fastForward"),
                // Fast-forward speed is configurable now (Ps1Settings.fastForwardSpeed, offered as
                // 1.5x/2x/3x/4x/Unlimited), so read the live label rather than naming a fixed
                // multiple — this row used to hardcode 2x back when the core capped it there.
                if (MainActivityRuntime.fastForwardToggleActive) {
                    str("action.fastForward.on") + " · ${com.armsx2.ui.GameOsd.fastForwardLabel}"
                } else {
                    "Resume at ${com.armsx2.ui.GameOsd.fastForwardLabel} speed"
                },
                "⏩",
                if (MainActivityRuntime.fastForwardToggleActive) Success else null,
            ) { MainActivityRuntime.instance?.toggleFastForward(); viewModel.resume() },
            // resetGame, NOT restart: restart() tears the VM down and relaunches, which on this port
            // lands the user back in the library. resetGame() uses the core's own in-place reset
            // (psxe_host_request_reset). NOTE there are TWO entry points to this action — this
            // row's onClick and EmulationMenuViewModel.activateSelection() index 2 for pad nav.
            // Changing only one leaves the bug alive on the other.
            MenuAction(str("memcard.restart"), str("action.reset"), "↻", null, MainActivityRuntime::resetGame),
            MenuAction(str("action.swapDisc"), str("action.swapDisc.detail"), "⏏", null, MainActivityRuntime::promptSwapDisc),
            // Multi-disc: the discs of the .m3u this session launched from. Empty for a single
            // disc game, in which case Swap Disc still opens the file picker as before.
            *MainActivityRuntime.playlistDiscs().map { disc ->
                MenuAction(
                    disc.label,
                    if (disc.exists) disc.file.name else "missing",
                    "💿",
                    null,
                ) { MainActivityRuntime.swapToPlaylistDisc(disc) }
            }.toTypedArray(),
            MenuAction(str("action.close"), MainActivityRuntime.currentGame.value?.title.orEmpty(), "■", Danger) {
                MainActivityRuntime.closeGame()
            },
            // Screenshot: the core writes a clean capture of the emulated frame (no touch overlay,
            // no OSD, no letterboxing) — see Screenshots.capture / psxe_host_request_screenshot.
            // Confirmation lands on the in-game OSD rather than an Android Toast so it is visible
            // over the game once the menu closes.
            MenuAction("Screenshot", "Clean capture, no overlay", "▣", null) {
                MainActivityRuntime.instance?.let { com.armsx2.Screenshots.capture(it.applicationContext) }
                com.armsx2.ui.GameOsd.toast("Screenshot saved")
                viewModel.resume()
            },
            // Rewind, one snapshot per press. The hold-to-rewind hotkey (Settings › Hotkeys)
            // is the real way to use this, but a touch-only player has no button to hold, so
            // there has to be a reachable control — a feature whose only entry point is an
            // unbound hotkey is a feature nobody finds. Stays in the menu so presses can be
            // repeated. Shown only while [emulation] rewind is actually on; with it off there
            // is no buffer and the row could only ever say "nothing to rewind to".
            *(if (state.rewindEnabled) {
                arrayOf(
                    MenuAction("Rewind", "Step back ${state.rewindStepLabel}", "⏪", null) {
                        viewModel.rewindStepBack()
                    }
                )
            } else {
                emptyArray()
            }),
        ),
        selected = state.selectedAction,
        onSelect = viewModel::selectAction,
    )
    // On-screen display — a single universal on/off (old-UI style); the per-stat
    // toggles live in All Settings. Plus a frame-limit switch so fast-forward is one
    // tap away.
    SectionCard(str("tab.overlay")) {
        // #357: the pause button replaced the settings cog, so it's front-and-centre here. This is
        // "tap to reveal", NOT show/hide: on = the glyph stays hidden until you tap its top-right
        // corner, which surfaces it. Either way that corner always opens this menu, so unlike the
        // old on/off toggle there's no setting here that can lock you out of it.
        MenuSwitchRow(str("pad.pauseTapToReveal.label"), TouchControls.pauseTapToReveal.value) {
            TouchControls.setPauseTapToReveal(it)
        }
        Spacer(Modifier.height(6.dp))
        // OSD mode selector — one control (Full / Minimal / Custom / Off) in place of the old
        // master + simple toggles, cycled here and by the "Cycle Perf Stats (OSD)" hotkey. Custom
        // = the detailed per-stat selection from All Settings > On-Screen.
        val osdModes = com.armsx2.ui.InGameOverlay.OsdMode.entries
        val osdModeIndex = osdModes.indexOf(com.armsx2.ui.InGameOverlay.osdMode.value).coerceAtLeast(0)
        MenuCycleRow(
            title = str("overlay.master.label"),
            valueLabel = com.armsx2.ui.InGameOverlay.osdModeLabel(osdModes[osdModeIndex]),
        ) { step ->
            val size = osdModes.size
            val next = ((osdModeIndex + step) % size + size) % size
            com.armsx2.ui.InGameOverlay.setOsdMode(osdModes[next])
        }
        Spacer(Modifier.height(6.dp))
        // Frame limit and the fast-forward speed, both straight through to the PS1 core's frame
        // pacer (Ps1Pacing → NativeApp.setSpeedLimits → ArmsxSession::targetFrameRate). This
        // switch used to write Settings.frameLimitEnable, whose native side was two empty PCSX2
        // stubs; the FF-speed row next to it was removed entirely for the same reason, back when
        // the core had exactly one hard-coded 2x. Both are real now.
        val pacingContext = androidx.compose.ui.platform.LocalContext.current.applicationContext
        MenuSwitchRow(str("perf.frameLimit.label"), com.armsx2.ui.InGameOverlay.frameLimitOn.value) { value ->
            com.armsx2.ui.InGameOverlay.frameLimitOn.value = value
            com.armsx2.core.Ps1Pacing.setFrameLimit(pacingContext, value)
        }
        Spacer(Modifier.height(6.dp))
        val ffSpeeds = com.armsx2.config.Ps1Settings.FAST_FORWARD_SPEEDS
        val ffSpeedIndex = ffSpeeds.indexOf(com.armsx2.core.Ps1Pacing.fastForwardSpeed(pacingContext))
            .takeIf { it >= 0 } ?: ffSpeeds.indexOf(2f)
        MenuCycleRow(
            title = str("perf.fastForwardSpeed.label"),
            valueLabel = com.armsx2.config.Ps1Settings.fastForwardSpeedLabel(ffSpeeds[ffSpeedIndex]),
        ) { step ->
            val size = ffSpeeds.size
            val next = ((ffSpeedIndex + step) % size + size) % size
            com.armsx2.core.Ps1Pacing.setFastForwardSpeed(pacingContext, ffSpeeds[next])
            com.armsx2.ui.GameOsd.fastForwardLabel =
                com.armsx2.config.Ps1Settings.fastForwardSpeedLabel(ffSpeeds[next])
        }
        Spacer(Modifier.height(6.dp))
        // OSD colour, cycled in place. Shares the palette with the All Settings picker rather
        // than carrying its own copy. Safe to add here: this card's rows are plain switches with
        // their own callbacks — SessionPane's selectedAction indexes the action GRID above, not
        // these, so inserting a row can't shift the controller dispatch.
        val osdColorIndex = com.armsx2.ui.settings.OSD_COLORS
            .indexOf(state.settings.osdColor).coerceAtLeast(0)
        MenuCycleRow(
            title = str("overlay.osdColor.label"),
            valueLabel = str(com.armsx2.ui.settings.OSD_COLOR_LABEL_KEYS[osdColorIndex]),
        ) { step ->
            val size = com.armsx2.ui.settings.OSD_COLORS.size
            val next = ((osdColorIndex + step) % size + size) % size
            viewModel.updateSettings { it.copy(osdColor = com.armsx2.ui.settings.OSD_COLORS[next]) }
        }
    }
    SectionCard(str("savestate.title.loadManage")) {
        Text(
            "${str("memcard.slot1").substringBefore(' ')} ${state.saveSlot + 1}",
            style = MaterialTheme.typography.titleMedium,
            color = MaterialTheme.colorScheme.onSurface,
        )
        Spacer(Modifier.height(8.dp))
        Row(
            Modifier
                .fillMaxWidth()
                .bleedHorizontal(13.dp)
                .horizontalScroll(rememberScrollState())
                .padding(horizontal = 13.dp),
            horizontalArrangement = Arrangement.spacedBy(6.dp),
        ) {
            repeat(10) { slot ->
                OptionChip(
                    label = "${slot + 1}",
                    selected = slot == state.saveSlot,
                    controllerId = "pause.saveslot.$slot",
                    onClick = { viewModel.setSaveSlot(slot) },
                )
            }
        }
        Spacer(Modifier.height(9.dp))
        // Save / Load open the rich slot picker (thumbnails + autosave + the
        // auto-save/-load toggles), matching the old UI. The slot chips above stay
        // the quick-slot selector used by the on-screen / hotkey quick-save.
        Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            CompactAction(str("savestate.title.save"), "↥", Modifier.weight(1f)) {
                com.armsx2.ui.WindowImpl.openInGameScreen(com.armsx2.ui.InGameScreen.SaveState)
            }
            CompactAction(str("touch.stateAction.load"), "↧", Modifier.weight(1f)) {
                com.armsx2.ui.WindowImpl.openInGameScreen(com.armsx2.ui.InGameScreen.LoadState)
            }
        }
    }
}

@Composable
private fun GraphicsPane(state: EmulationMenuUiState, viewModel: EmulationMenuViewModel) {
    // The PS2 Settings object survives in this pane for exactly one thing — the RetroArch shader
    // chain, which is still stored there. Every emulator-facing row below reads and writes
    // Ps1Settings / settings.toml instead.
    val shaderSettings = state.settings
    val context = androidx.compose.ui.platform.LocalContext.current

    // ONE renderer picker, shared with full Settings → Video. Both read and write
    // Ps1Settings.gpuBackend (settings.toml [video] gpu_backend), which is the only thing the
    // PS1 core actually reads.
    //
    // What used to be here was `Settings.renderer` — a lifted PCSX2 control (auto/vulkan/
    // opengl/software backed by EmuCore/GS/Renderer). Nothing in this core has ever read it,
    // so every selection made here was inert, and it disagreed with the Video tab in labels,
    // option set and order. "Auto" is gone with it: it was a PCSX2 concept with no behaviour
    // here (the core's own fallback ladder is not user-selectable), and shipping a dead option
    // is worse than shipping none.
    // ★ ACTIVE, not global: what this pane shows has to be what the RUNNING game is using, which is
    // the global layer merged with anything that game pins for itself (Ps1SettingsStore.load's
    // defaults < global < per-game). Showing the raw global here would print one value while the
    // core ran another.
    var ps1 by remember { mutableStateOf(com.armsx2.config.Ps1SettingsStore.active(context)) }

    // ── ONE writer for every PS1 row in this menu ──────────────────────────────────────────
    //
    // Writes the GLOBAL layer — the pause menu has always meant "my default", and quietly turning
    // every in-game tweak into a per-game pin would make later global edits stop working for every
    // game the user ever paused. The per-game layer is edited from the game's own settings screen,
    // which badges each row with the tier that wins.
    //
    // The transform therefore runs against GLOBAL, while the state fed back to the UI is re-read
    // from the ACTIVE scope: if this game pins the key that was just changed, the row snaps back to
    // the pinned value instead of lying about having applied. Ps1PerGameNotice below names those
    // keys and offers to drop them, so the "why did nothing happen" question is answered on screen.
    val editPs1: ((com.armsx2.config.Ps1Settings) -> com.armsx2.config.Ps1Settings) -> Unit = { transform ->
        runCatching { com.armsx2.config.Ps1SettingsStore.update(context, null, transform) }
        ps1 = runCatching { com.armsx2.config.Ps1SettingsStore.active(context) }.getOrDefault(ps1)
        // A full-file rewrite moves nothing in [runtime], but Ps1Pacing caches that table, so
        // drop and re-push it rather than leave a cache that could disagree with the file.
        com.armsx2.core.Ps1Pacing.invalidate()
        com.armsx2.core.Ps1Pacing.push(context)
        com.armsx2.core.Ps1Display.push(context)
        com.armsx2.core.Ps1Emulation.push(context)
    }

    com.armsx2.ui.settings.Ps1PerGameNotice(context)

    HorizontalOptions(
        title = str("tab.renderer"),
        options = com.armsx2.config.Ps1Settings.GPU_BACKENDS
            .zip(com.armsx2.config.Ps1Settings.GPU_BACKEND_LABELS),
        selected = ps1.gpuBackend,
        onSelect = { token -> editPs1 { it.copy(gpuBackend = token) } },
    )

    // What is GENUINELY running, straight from the renderer that survived the fallback ladder
    // — not what was picked above. This is the only way a user can tell that e.g. ANGLE was
    // requested but the system driver loaded, or that a custom Vulkan driver silently didn't.
    val active = remember(ps1.gpuBackend) {
        runCatching { kr.co.iefriends.pcsx2.NativeApp.getActiveRenderer() }.getOrDefault("")
    }
    if (active.isNotBlank()) {
        Text(
            "Active: $active",
            style = MaterialTheme.typography.bodySmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )
    }

    // The custom-driver managers, matched to the backend actually selected above. Vulkan gets
    // the adrenotools driver pack manager; the GLES backends get the ANGLE picker, which is
    // the same choice as the "OpenGL ES (ANGLE)" entry in the list above — kept here because
    // this pane is also where the driver downloads live.
    if (ps1.gpuBackend == com.armsx2.config.Ps1Settings.GPU_VULKAN) {
        com.armsx2.ui.common.DriverManagerSection()
    } else if (ps1.gpuBackend == com.armsx2.config.Ps1Settings.GPU_OPENGL ||
        ps1.gpuBackend == com.armsx2.config.Ps1Settings.GPU_ANGLE
    ) {
        com.armsx2.ui.common.AngleDriverSection(
            ps1.gpuBackend == com.armsx2.config.Ps1Settings.GPU_ANGLE,
        ) { on ->
            editPs1 {
                it.copy(
                    gpuBackend = if (on) com.armsx2.config.Ps1Settings.GPU_ANGLE
                    else com.armsx2.config.Ps1Settings.GPU_OPENGL,
                )
            }
            runCatching { kr.co.iefriends.pcsx2.NativeApp.setGlDriver(if (on) "angle" else "system") }
        }
    }
    // "GS Multi-threading" and "Coalesce render passes" used to sit here. Both were PCSX2
    // Graphics-Synthesizer controls with no PlayStation counterpart: the first split the GS
    // between a front and a back thread, the second grouped consecutive GS draws to the same
    // render target into one pass. The ARMSX1 core has no GS — its rasteriser is a plain
    // software/hardware pair driven from the emulation thread — so both rows toggled a field
    // nothing reads.
    CompactAction(str("backend.applyRestart"), "↻", Modifier.fillMaxWidth(), MainActivityRuntime::restart)

    // ── Everything below writes settings.toml, the only configuration this core reads ──
    // through the single `editPs1` writer declared at the top of this pane. Live-apply goes through
    // the existing Ps1Display helper; there is no second mechanism, and nothing here goes near
    // NativeApp.setSetting / commitSettings / render*, which are empty PCSX2 stubs in this port.
    Text(
        "Display mode and Stretch apply to the running game immediately. Upscale, filtering, " +
            "VSync and the accuracy flags are read when a game starts — change them, then Apply / Restart.",
        style = MaterialTheme.typography.bodySmall,
        color = MaterialTheme.colorScheme.onSurfaceVariant,
    )

    // UPSCALE — the real one, and half of the original report.
    //
    // This row used to write Settings.upscaleFloat and call NativeApp.renderUpscalemultiplier(),
    // a PCSX2 entry point with an empty body here: eighteen chips from 0.25x to 8x, none of which
    // reached the core. The PlayStation lever is [video] renderer + internal_scale — `software`
    // rasterises at the console's own resolution, `hardware` is the scale-aware rasteriser that
    // can render above it. Native therefore means "software rasteriser": at 1x the hardware one is
    // pixel-identical and still costs roughly twice the CPU, so there is no reason to select it.
    // Both keys are read when a SESSION STARTS (ArmsxSession → armsx_hw_gl_create), which is why
    // the Apply / Restart action sits directly above rather than below.
    HorizontalOptions(
        title = str("renderer.upscale.label"),
        options = listOf(1 to "Native") + (2..8).map { it to "${it}x" },
        selected = if (ps1.hwRasterizer) ps1.internalScale.coerceIn(1, 8) else 1,
        onSelect = { scale -> editPs1 { it.copy(hwRasterizer = scale > 1, internalScale = scale) } },
    )

    // DISPLAY MODE — the other half of the report. It used to write Settings.aspectRatio, whose
    // only native call is the empty NativeApp.setAspectRatio() stub. The PlayStation core presents
    // through [video] stretch_mode + display_aspect, and unlike the upscale above this is pure
    // presentation, so Ps1Display.push lands it on the next presented frame of the running game.
    // Stretch is folded in as a mode rather than a separate switch because it overrides the ratio
    // outright — as two controls, "Stretch on + 4:3" reads like a contradiction.
    val aspects = com.armsx2.config.Ps1Settings.ASPECTS
    HorizontalOptions(
        title = str("renderer.displayMode.label"),
        options = listOf(-1 to str("setup.aspect.stretch")) +
            aspects.indices.map { it to com.armsx2.config.Ps1Settings.ASPECT_LABELS[it] },
        selected = if (ps1.stretchMode) -1 else aspects.indexOf(ps1.displayAspect).coerceAtLeast(0),
        onSelect = { index ->
            editPs1 {
                if (index < 0) it.copy(stretchMode = true)
                else it.copy(stretchMode = false, displayAspect = aspects[index])
            }
        },
    )
    if (!ps1.stretchMode && ps1.displayAspect == com.armsx2.config.Ps1Settings.ASPECT_CUSTOM) {
        // Matched with a tolerance, never ==: settings.toml stores the ratio to four decimals, so
        // 21:9 comes back as 2.3703 and an exact Float compare would show no chip selected.
        val presets = com.armsx2.config.Ps1Settings.ASPECT_CUSTOM_PRESETS
        HorizontalOptions(
            title = str("renderer.customAspect.label"),
            options = presets.indices.map { it to presets[it].first },
            selected = presets.indexOfFirst {
                kotlin.math.abs(it.second - ps1.displayAspectCustom) < 0.001f
            },
            onSelect = { index -> editPs1 { it.copy(displayAspectCustom = presets[index].second) } },
        )
    }

    // Blending Accuracy, Texture Filtering, Texture Preloading, Hardware Download Mode and
    // Hardware Mipmapping were removed here: every one is a level of the PS2's Graphics
    // Synthesizer — its texture cache, its alpha blend unit, its readback path, its mip chain.
    // The PlayStation GPU has a fixed blend function, no texture cache, no readback and no
    // mipmapping at all. Deinterlacing went with them: this core never produces an interlaced
    // frame (there is no field handling anywhere in psx/), so there was nothing to weave or bob.
    // The one filtering choice that IS real on PS1 is nearest vs linear, and it is this row.
    HorizontalOptions(
        title = str("renderer.displayFilter.label"),
        // Two, not three: "Smooth" and "Sharp" were two PCSX2 bilinear variants of a GS filter.
        // [video] texture_scale_mode is a plain bool — the same key the Video tab writes as
        // "Bilinear filtering".
        options = listOf(
            false to str("renderer.textureFilter.nearest"),
            true to str("renderer.textureFilter.bilinear"),
        ),
        selected = ps1.textureScaleMode,
        onSelect = { linear -> editPs1 { it.copy(textureScaleMode = linear) } },
    )
    MenuSwitchRow(str("renderer.vsync.label"), ps1.vsync) { v -> editPs1 { it.copy(vsync = v) } }

    // The PlayStation GPU's own accuracy flags, both real [video] keys and both off by default
    // because both change what is drawn. This slot used to hold PCSX2's four-level Dithering
    // (Off / Scaled / Unscaled / Force 32bit), which describes the GS's 32-bit framebuffer; the
    // PlayStation dithers 24-bit colour down to 15-bit and the only question is whether the
    // hardware's gating of that is emulated. Mask bit is the "don't draw over protected pixels"
    // bit games use for HUD and shadow tricks.
    MenuSwitchRow(
        str("fixes.dithering.label"),
        ps1.accurateDither,
        description = str("fixes.dithering.description"),
    ) { v -> editPs1 { it.copy(accurateDither = v) } }
    MenuSwitchRow(
        str("fixes.maskBit.label"),
        ps1.accurateMaskBit,
        description = str("fixes.maskBit.description"),
    ) { v -> editPs1 { it.copy(accurateMaskBit = v) } }

    // Kept, deliberately inert. Whole-number scaling is a genuine want for a PlayStation image —
    // it is what keeps 320x240 pixels square on a 1080p panel — but the core's present path has
    // no such option: armsx_render_frame_params_t carries stretch, linear_filter and aspect and
    // nothing else. Shown disabled rather than deleted so the capability is not quietly lost.
    MenuSwitchRow(
        str("fixes.integerScaling.label"),
        checked = false,
        enabled = false,
        description = str("fixes.integerScaling.description"),
    ) { }

    // Shadeboost, Sync To Host Refresh, Anti-Blur, Screen Offsets, Show Overscan and the three
    // texture-replacement switches were removed. Shadeboost is a PCSX2 post-process shader; Sync
    // To Host Refresh is its pacing scheme (this core paces through targetFrameRate + VSync); the
    // PCRTC ones configure the PS2's display compositor, which the PlayStation does not have; and
    // this core has no texture-replacement path at all. Colour/CRT looks are the shader chain's
    // job below.
    //
    // RetroArch shaders, end-to-end in-game: toggle → pick a preset → download more. Same
    // composables the settings hub renders (single definition in ui/common). The chain IS wired
    // now — librashader over the Vulkan present path, with shaderPresetParams /
    // setShaderChainParams as real natives — so the old "not rendered yet" note is gone. It ran
    // for a while after the feature landed, which is its own kind of bug: a UI that lies about
    // working is as bad as one that silently does nothing.
    Text(
        "Runs the selected preset over the emulated image. The chain reads at the PlayStation's " +
            "native resolution regardless of internal resolution, so scanlines and masks stay the " +
            "right size instead of scaling with the upscale.",
        style = MaterialTheme.typography.bodySmall,
        color = MaterialTheme.colorScheme.onSurfaceVariant,
    )
    com.armsx2.ui.common.ShaderChainSection(
        enabled = shaderSettings.shaderChainEnabled,
        preset = shaderSettings.shaderChainPreset,
        params = shaderSettings.shaderChainParams,
        onEnabledChange = { on -> viewModel.updateSettings { it.copy(shaderChainEnabled = on) } },
        onPresetChange = { path -> viewModel.updateSettings { it.copy(shaderChainPreset = path) } },
        onParamsChange = { next -> viewModel.updateSettings { it.copy(shaderChainParams = next) } },
    )
    com.armsx2.ui.common.ShaderManagerSection()
}

@Composable
private fun PerformancePane(state: EmulationMenuUiState, viewModel: EmulationMenuViewModel) {
    val settings = state.settings
    val context = androidx.compose.ui.platform.LocalContext.current.applicationContext

    // Frame pacing — settings.toml's [runtime] table, pushed live through
    // NativeApp.setSpeedLimits() into ArmsxSession::targetFrameRate().
    //
    // All three rows in this block used to write PS2 Settings fields (frameLimitEnable,
    // nominalSpeedPercent, fpsLimit) whose native side — speedhackLimitermode / setNominalSpeed /
    // setFpsCap — is an empty stub in this port. They persisted and never reached the emulation
    // thread, which is the "display FPS cap does nothing" report in its original form. Ps1Pacing
    // owns the policy and writes the file itself; these rows only hand it one value and re-read.
    var pacing by remember { mutableStateOf(com.armsx2.core.Ps1Pacing.settings(context)) }
    SectionCard(str("perf.speedLimit.label")) {
        Row(verticalAlignment = Alignment.CenterVertically) {
            Text(
                if (pacing.frameLimit) "${pacing.speedPercent}%" else str("setup.toggle.off"),
                modifier = Modifier.weight(1f),
                color = MaterialTheme.colorScheme.primary,
                style = MaterialTheme.typography.titleMedium,
                fontWeight = FontWeight.Bold,
            )
            Switch(
                checked = pacing.frameLimit,
                onCheckedChange = { enabled ->
                    com.armsx2.core.Ps1Pacing.setFrameLimit(context, enabled)
                    // Same switch as the Session tab's and the touch overlay's; that one reads
                    // this mirror, so leaving it stale would show two different answers.
                    com.armsx2.ui.InGameOverlay.frameLimitOn.value = enabled
                    pacing = com.armsx2.core.Ps1Pacing.settings(context)
                },
            )
        }
        Spacer(Modifier.height(8.dp))
        HorizontalOptionRow(
            options = com.armsx2.config.Ps1Settings.SPEED_PERCENTS.map { it to "$it%" },
            selected = pacing.speedPercent,
            keyPrefix = str("perf.speedLimit.label"),
            onSelect = { percent ->
                com.armsx2.core.Ps1Pacing.setSpeedPercent(context, percent)
                pacing = com.armsx2.core.Ps1Pacing.settings(context)
            },
        )
    }
    HorizontalOptions(
        title = str("perf.displayFpsCap.label"),
        options = com.armsx2.config.Ps1Settings.FPS_LIMITS.map {
            it to if (it == 0) str("setup.toggle.off") else "$it FPS"
        },
        selected = pacing.fpsLimit,
        onSelect = { fps ->
            com.armsx2.core.Ps1Pacing.setFpsLimit(context, fps)
            pacing = com.armsx2.core.Ps1Pacing.settings(context)
        },
    )
    // Frame skip. Real now: [runtime] frame_skip, pushed through the same Ps1Pacing call as the
    // rows above and consumed by ArmsxApp::shouldSkipPresent(). It drops PRESENTS only — the
    // machine is still stepped to every vblank, so the game's timing and the SPU's per-frame
    // sample pull are untouched and it is not a speed control. (The row was a switch wired to
    // NativeApp.setFrameSkip(), an empty stub, and then a disabled placeholder.)
    HorizontalOptions(
        title = str("perf.frameSkip.label"),
        options = com.armsx2.config.Ps1Settings.FRAME_SKIPS.map {
            it to com.armsx2.config.Ps1Settings.frameSkipLabel(it)
        },
        selected = pacing.frameSkip,
        onSelect = { skip ->
            com.armsx2.core.Ps1Pacing.setFrameSkip(context, skip)
            pacing = com.armsx2.core.Ps1Pacing.settings(context)
        },
    )
    Text(
        str("perf.frameSkip.description"),
        style = MaterialTheme.typography.bodySmall,
        color = MaterialTheme.colorScheme.onSurfaceVariant,
    )
    // The NTSC / PAL framerate sliders and Skip Duplicate Frames went with the rest. The two
    // sliders retuned PCSX2's emulated vertical rate, a value this core derives from the disc's
    // region and then scales by Speed Limit above; Skip Duplicate Frames is a GS present-path
    // optimisation with no counterpart here. All three wrote fields nothing reads.
    //
    // Everything that used to follow was PS2 silicon, and none of it has a PlayStation analogue:
    //   EE Cycle Rate / Cycle Skip      — underclock hacks for the Emotion Engine (R5900)
    //   EE/FPU + VU clamping and rounding — the EE FPU and the two Vector Units' float behaviour
    //   MTVU, Instant VU1, VU Flag Hack — Vector Unit speedhacks
    //   Fast CDVD                       — the PS2's CD/DVD block
    //   INTC Stat / Wait Loop Detection — EE idle-loop speedhacks
    //   Recompiler card (EE R5900 / IOP R3000 / VU0 / VU1 / Fastmem) — PCSX2's five JITs
    // ---- Device performance ------------------------------------------------------------
    //
    // These four are HOST levers — they act on the phone, not on the emulated console — and all
    // four were already fully implemented in this port and simply had NO UI: EmulationSurface
    // reads hwScaler, screenResOverride and vsyncQueueSize, and MainActivityRuntime applies
    // sustained performance at launch. The settings persisted and the behaviour was live; there
    // was just no way to reach any of it.
    //
    // Two more from the same ARMSX2 group were absent here while their natives were empty stubs.
    // Both are implemented now (frontend/perf_hint.c) and both appear below AND on Settings ›
    // Graphics next to Sustained performance. In-game matters for these two specifically: they
    // are levers whose only honest test is an A/B in a scene that will not hold full speed, and
    // leaving the game to flip one loses the scene.
    SectionCard(str("perf.section.device")) {
        // Zero queued frames + a display refresh vote that is an integer multiple of the game's
        // rate (60 -> 120 Hz, PAL 50 -> 100 Hz), so there is no 3:2 cadence.
        MenuSwitchRow(str("renderer.lowLatency.label"), settings.vsyncQueueSize == 0) { enabled ->
            viewModel.updateSettings { it.copy(vsyncQueueSize = if (enabled) 0 else 2) }
            runCatching {
                com.armsx2.runtime.MainActivityRuntime.surface.value?.applyFrameRatePreference()
            }
        }
        Spacer(Modifier.height(6.dp))
        // Shrinks the OUTPUT surface and lets the display composer scale it back up: cuts GPU
        // present cost, heat and battery without touching the emulated resolution. Steps are
        // multiples of the PlayStation's native 240 lines.
        run {
            val labels = listOf("Screen", "3x native", "2x native", "1x native")
            val toIndex = when (settings.hwScaler) { 3 -> 1; 2 -> 2; 1 -> 3; else -> 0 }
            MenuCycleRow("Display resolution", labels[toIndex]) { step ->
                val next = ((toIndex + step) % labels.size + labels.size) % labels.size
                viewModel.updateSettings {
                    it.copy(hwScaler = when (next) { 1 -> 3; 2 -> 2; 3 -> 1; else -> 0 })
                }
                runCatching {
                    com.armsx2.runtime.MainActivityRuntime.surface.value?.applyOutputScale()
                }
            }
        }
        Spacer(Modifier.height(6.dp))
        // Forces the output surface to a fixed size instead of the detected panel, for panels
        // that mis-report (a 1080p screen claiming 1920x1200 squishes the image).
        run {
            val presets = listOf("auto", "2560x1440", "1920x1080", "1280x720")
            val labels = listOf("Auto", "1440p", "1080p", "720p")
            val idx = presets.indexOf(settings.screenResOverride).let { if (it >= 0) it else 0 }
            MenuCycleRow("Screen resolution", labels[idx]) { step ->
                val next = ((idx + step) % presets.size + presets.size) % presets.size
                viewModel.updateSettings { it.copy(screenResOverride = presets[next]) }
                runCatching {
                    com.armsx2.runtime.MainActivityRuntime.surface.value?.applyOutputScale()
                }
            }
        }
        Spacer(Modifier.height(6.dp))
        // Asks Android to hold a steady thermally-sustainable clock rather than boost-then-throttle.
        // Better over a long session, but it CAPS peak clock, so a demanding game can lose fps.
        // A raw pref, not a Settings field, because it is device-wide rather than per-game.
        run {
            val prefs = com.armsx2.runtime.MainActivityRuntime.prefs
            var sustained by remember { mutableStateOf(prefs.getBoolean("ui.sustainedPerf", false)) }
            MenuSwitchRow(str("renderer.sustainedPerf.label"), sustained) { enabled ->
                sustained = enabled
                prefs.edit().putBoolean("ui.sustainedPerf", enabled).apply()
                if (android.os.Build.VERSION.SDK_INT >= android.os.Build.VERSION_CODES.N) {
                    runCatching {
                        // Via the SURFACE's context, not this pane's `context` — that one is the
                        // applicationContext, so an Activity cast on it is always null and the
                        // toggle would persist correctly while doing nothing at all.
                        (com.armsx2.runtime.MainActivityRuntime.surface.value?.context
                            as? android.app.Activity)
                            ?.window?.setSustainedPerformanceMode(enabled)
                    }
                }
            }
        }
        Spacer(Modifier.height(6.dp))
        // The opposite lever to the one above: Sustained performance CAPS the clock, this asks
        // for the clock the frame's work needs. Native reports the emulation thread's WORK per
        // frame — not the frame's wall clock, which includes the limiter's sleep and would read
        // as permanent max demand. EXPERIMENTAL, off by default, Android 13+; a raw pref because
        // it is device-wide rather than per-game, same as Sustained performance.
        run {
            val prefs = com.armsx2.runtime.MainActivityRuntime.prefs
            var adpf by remember { mutableStateOf(prefs.getBoolean("ui.adpf", false)) }
            MenuSwitchRow(str("renderer.adpf.label"), adpf) { enabled ->
                adpf = enabled
                prefs.edit().putBoolean("ui.adpf", enabled).apply()
                runCatching { kr.co.iefriends.pcsx2.NativeApp.setAdpfEnabled(enabled) }
            }
        }
        Spacer(Modifier.height(6.dp))
        // Restricts the emulation thread to a core group. The performance cluster is detected
        // from the device's own clock table, never assumed from core index. Applies live.
        run {
            val labels = listOf(
                str("common.off"),
                str("renderer.affinity.performanceCores"),
                str("renderer.affinity.allCores"),
            )
            val toIndex = when (settings.affinityMode) { 1, 7 -> 1; 2 -> 2; else -> 0 }
            MenuCycleRow(str("renderer.affinity.label"), labels[toIndex]) { step ->
                val next = ((toIndex + step) % labels.size + labels.size) % labels.size
                viewModel.updateSettings { it.copy(affinityMode = next) }
                runCatching { kr.co.iefriends.pcsx2.NativeApp.setAffinityMode(next) }
            }
        }
    }
    Spacer(Modifier.height(10.dp))
    // The PlayStation has one CPU (R3000A) plus the GTE, and this core's only execution choice is
    // Cached vs Interpreter — which is on Settings › Emulation, written to settings.toml [cpu].
    SectionCard(str("tab.overlay")) {
        MenuSwitchRow(str("overlay.toggle.fps"), settings.osdShowFps) { value ->
            viewModel.updateSettings { it.copy(osdShowFps = value) }
        }
        Spacer(Modifier.height(6.dp))
        MenuSwitchRow(str("overlay.toggle.emulationSpeed"), settings.osdShowSpeed) { value ->
            viewModel.updateSettings { it.copy(osdShowSpeed = value) }
        }
        Spacer(Modifier.height(6.dp))
        Spacer(Modifier.height(6.dp))
        // Statistics rows. Labels are written out rather than pulled from i18n because the
        // inherited keys name PS2 hardware ("CPU usage", "GS statistics") while these flags now
        // drive PS1 counters — see the mapping note in com.armsx2.ui.GameOsd. Each one arms real
        // instrumentation inside the emulator, so they stay individually toggleable and off by
        // default; with all four clear the core carries no counters at all.
        MenuSwitchRow("R3000A + GTE", settings.osdShowCpu) { value ->
            viewModel.updateSettings { it.copy(osdShowCpu = value) }
        }
        Spacer(Modifier.height(6.dp))
        MenuSwitchRow(str("overlay.toggle.gpuPrimitives"), settings.osdShowGpu) { value ->
            viewModel.updateSettings { it.copy(osdShowGpu = value) }
        }
        Spacer(Modifier.height(6.dp))
        MenuSwitchRow("SPU / MDEC / CD-ROM / DMA", settings.osdShowGsStats) { value ->
            viewModel.updateSettings { it.copy(osdShowGsStats = value) }
        }
        Spacer(Modifier.height(6.dp))
        MenuSwitchRow(str("overlay.toggle.frameTimes"), settings.osdShowFrameTimes) { value ->
            viewModel.updateSettings { it.copy(osdShowFrameTimes = value) }
        }
        Spacer(Modifier.height(6.dp))
        MenuSwitchRow(str("overlay.toggle.internalResolution"), settings.osdShowResolution) { value ->
            viewModel.updateSettings { it.copy(osdShowResolution = value) }
        }
        Spacer(Modifier.height(6.dp))
        MenuSwitchRow(str("tab.renderer"), settings.osdShowHardwareInfo) { value ->
            viewModel.updateSettings { it.copy(osdShowHardwareInfo = value) }
        }
        Spacer(Modifier.height(6.dp))
        MenuSwitchRow(str("overlay.toggle.deviceUsage"), settings.osdShowHostUsage) { value ->
            viewModel.updateSettings { it.copy(osdShowHostUsage = value) }
        }
        Spacer(Modifier.height(6.dp))
        MenuSwitchRow(str("overlay.toggle.onScreenNotifications"), settings.osdShowMessages) { value ->
            viewModel.updateSettings { it.copy(osdShowMessages = value) }
        }
    }
}

@Composable
private fun ControlsPane(state: EmulationMenuUiState, viewModel: EmulationMenuViewModel) {
    MenuSwitchRow(str("pad.onScreenControls.label"), state.touchControlsVisible) {
        viewModel.toggleTouchControls()
    }
    MenuSwitchRow(
        title = str("pad.rumble.label"),
        checked = state.rumbleEnabled,
        onCheckedChange = viewModel::setRumble,
    )
    // Vibration Strength — the same global 0-200% haptic multiplier as All Settings ›
    // Controls, reachable here in-game. Local state drives the live update since it's a
    // plain pref (not part of EmulationMenuUiState).
    var haptic by remember { mutableStateOf(com.armsx2.input.ControllerMappings.hapticIntensity()) }
    com.armsx2.ui.settings.IntSliderRow(
        label = str("pad.hapticStrength.label"),
        value = haptic,
        min = 0,
        max = 200,
        description = str("pad.hapticStrength.description"),
        valueFormatter = { if (it == 0) "Off" else "${it}%" },
        onChange = { haptic = it; com.armsx2.input.ControllerMappings.setHapticIntensity(it) },
    )
    MenuSwitchRow(str("pad.multitap.label"), state.multitapEnabled, onCheckedChange = viewModel::setMultitap)
    // "Emulate USB keyboard" was removed: it attached a USB HID keyboard to one of the PS2's two
    // USB ports for the handful of PS2 titles that took keyboard input. The PlayStation has no USB
    // bus and no keyboard peripheral, so there was no device for the switch to plug in.

    // Gesture control, in-game. Worth having here rather than only in All Settings: the swipe
    // distance and the Tap/Hold choice are things you only discover the right value for while
    // actually playing, and walking out to the settings tree to nudge them loses the moment.
    // Local state, like the haptic slider above — these are plain prefs, not part of the ui state.
    var gestureOn by remember { mutableStateOf(TouchControls.gestureEnabled.value) }
    MenuSwitchRow(str("pad.gesture.enable.label"), gestureOn) {
        gestureOn = it
        TouchControls.setGestureEnabled(it)
    }
    if (gestureOn) {
        var swipeSens by remember { mutableStateOf((TouchControls.gestureSwipeSensitivity.floatValue * 100f).toInt()) }
        com.armsx2.ui.settings.IntSliderRow(
            label = str("pad.gesture.sensitivity.label"),
            value = swipeSens,
            min = 5,
            max = 60,
            description = str("pad.gesture.sensitivity.description"),
            valueFormatter = { "${it}%" },
            onChange = { swipeSens = it; TouchControls.setGestureSensitivity(it / 100f) },
        )
        var holdMode by remember { mutableStateOf(TouchControls.gestureDoubleTapHold.value) }
        HorizontalOptions(
            title = str("pad.gesture.doubleTapMode.label"),
            options = listOf(
                0 to str("pad.gesture.doubleTapMode.tap"),
                1 to str("pad.gesture.doubleTapMode.hold"),
            ),
            selected = if (holdMode) 1 else 0,
            onSelect = { holdMode = it == 1; TouchControls.setGestureDoubleTapHold(holdMode) },
        )
        // The four swipe/double-tap ASSIGNMENTS stay in All Settings — six button pickers would
        // swamp this pane, and you set them once rather than mid-session.
    }
    CompactAction(str("pad.controllerMapping"), "⌁", Modifier.fillMaxWidth(), viewModel::openControlsManager)
    Spacer(Modifier.height(6.dp))
    CompactAction(str("pad.editTouchLayout"), "✥", Modifier.fillMaxWidth(), viewModel::editTouchControls)
    Spacer(Modifier.height(6.dp))
    // Sits with the touch layout because it's the same job: what the on-screen pad LOOKS
    // like, right after where it's laid out. Full-screen like Controller mapping.
    CompactAction(str("tab.skins"), "◈", Modifier.fillMaxWidth(), viewModel::openSkins)
    // Motion / gyroscope controls in-game (mode, sensitivity, smoothing, invert). Global scope
    // to match the rumble/multitap toggles above; the per-game scope lives in All Settings › Controls.
    com.armsx2.ui.settings.GyroSection()
    // Macros — edit each M1-M4 button set here in-game too (physical-trigger binding stays
    // in All Settings › Controls, which hosts the key-capture listener).
    com.armsx2.ui.settings.MacrosSection()
}

@Composable
private fun OptionsPane(state: EmulationMenuUiState, viewModel: EmulationMenuViewModel) {
    val settings = state.settings
    // Gateway to the full settings screen — every category the compact menu omits
    // (Video, Emulation, BIOS, Library, Interface, Advanced).
    CompactAction(str("action.allSettings"), "⚙", Modifier.fillMaxWidth(), viewModel::openFullSettings)
    Spacer(Modifier.height(6.dp))
    // In-game access to the manager screens (the library drawer's Memory Cards /
    // Patches & Cheats / Controller mapping) — open over the paused game.
    Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.spacedBy(6.dp)) {
        CompactAction(str("memcard.title"), "▤", Modifier.weight(1f), viewModel::openMemcard)
        CompactAction(str("cheats.title"), "✦", Modifier.weight(1f), viewModel::openPatches)
    }
    Spacer(Modifier.height(6.dp))
    Spacer(Modifier.height(6.dp))
    // ONE cheat switch, and it is real.
    //
    // This used to be two rows driving Settings.enablePatches / Settings.enableCheats — PS2
    // EmuCore INI keys that are written to a PCSX2 settings layer this build does not have.
    // Both moved, saved, survived a restart and changed nothing whatsoever, which is exactly
    // the failure this project keeps shipping. The row below flips `[cheats] enabled` for the
    // RUNNING game through the same per-game layer the Cheats screen writes, and pushes it to
    // the core on the next frame.
    run {
        // applicationContext: this row outlives the composition that opened the menu.
        val ctx = androidx.compose.ui.platform.LocalContext.current.applicationContext
        val gameKey = com.armsx2.config.Ps1SettingsStore.activeGameKey
        // Subscribe to the per-game store so the switch reflects an edit made on the Cheats
        // screen without the menu being reopened.
        com.armsx2.config.Ps1GameSettings.version.intValue
        val cheatsOn = gameKey != null &&
            com.armsx2.cheats.Ps1Cheats.settingsFor(ctx, gameKey).cheatsEnabled
        MenuSwitchRow(
            if (state.hardcore) str("cheats.master.labelHardcore") else str("cheats.master.label"),
            cheatsOn && !state.hardcore,
            enabled = !state.hardcore && gameKey != null,
        ) { on ->
            if (gameKey != null) {
                val current = com.armsx2.cheats.Ps1Cheats.settingsFor(ctx, gameKey)
                com.armsx2.cheats.Ps1Cheats.setEnabled(ctx, gameKey, on, current.cheatsEnabledCodes)
            }
        }
    }
    // Reads and writes Ps1Settings.fastBoot, NOT the PS2-lineage Settings.enableFastBoot this row
    // used to drive. That field is persisted in seven places and has no native side whatsoever, so
    // the switch moved, saved, survived a restart — and skipped nothing. It defaulted to ON, which
    // is why it read as a working feature.
    run {
        // applicationContext: this row outlives the composition that opened the menu, and
        // Ps1SettingsStore only needs a files dir.
        val ctx = androidx.compose.ui.platform.LocalContext.current.applicationContext
        // Shows the ACTIVE (merged) answer, writes the GLOBAL layer — same split as the Display
        // pane's editPs1, and for the same reason. A game that pins Skip BIOS keeps its pin and the
        // switch snaps back to it rather than pretending the change took.
        val ps1Local = remember { mutableStateOf(com.armsx2.config.Ps1SettingsStore.active(ctx)) }
        MenuSwitchRow(str("perf.fix.skipBios"), ps1Local.value.fastBoot) { on ->
            runCatching { com.armsx2.config.Ps1SettingsStore.update(ctx, null) { it.copy(fastBoot = on) } }
            ps1Local.value = runCatching { com.armsx2.config.Ps1SettingsStore.active(ctx) }
                .getOrDefault(ps1Local.value)
        }
    }
    // The widescreen and no-interlacing switches, the GameDB "compatibility fixes" master and its
    // eight per-fix rows (Skip MPEG, FMV software renderer, EE Timing, Instant DMA, Blit Internal
    // FPS, VU Add/Sub, VU Sync) were removed. All of them are PCSX2 constructs:
    //   - widescreen / no-interlacing are PNACH patch CATEGORIES that only exist in the PS2 patch
    //     archive, keyed by PS2 serial + ELF CRC;
    //   - the gamefixes each name PS2 silicon (the Emotion Engine's timing, the GS blit path, the
    //     IPU's MPEG decoder, the Vector Units) and are looked up in PCSX2's GameDB, which this
    //     app does not ship.
    // There is no PlayStation equivalent to fix, so every one of them wrote a field nothing reads.
}

@Composable
private fun AchievementsPane(state: EmulationMenuUiState, viewModel: EmulationMenuViewModel) {
    // Gateway to the full RetroAchievements screen (unlock list + presentation options).
    CompactAction(str("ra.viewAchievements"), "★", Modifier.fillMaxWidth(), viewModel::openAchievements)
    Spacer(Modifier.height(4.dp))
    SectionCard("RetroAchievements") {
        // Signed-in account: avatar + name + both point totals (hardcore / softcore).
        if (state.raUserName.isNotBlank()) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                if (state.raAvatarUrl.isNotBlank()) {
                    AsyncImage(
                        state.raAvatarUrl,
                        state.raUserName,
                        Modifier.size(46.dp).clip(CircleShape),
                        contentScale = ContentScale.Crop,
                    )
                    Spacer(Modifier.width(12.dp))
                }
                Column(Modifier.weight(1f)) {
                    Text(
                        state.raUserName,
                        style = MaterialTheme.typography.titleSmall,
                        color = MaterialTheme.colorScheme.onSurface,
                    )
                    Row(verticalAlignment = Alignment.CenterVertically) {
                        Text(
                            "${state.raScore} HC",
                            style = MaterialTheme.typography.bodySmall,
                            color = com.armsx2.ui.theme.Danger,
                        )
                        Text(
                            "  ·  ${state.raSoftcoreScore} SC",
                            style = MaterialTheme.typography.bodySmall,
                            color = MaterialTheme.colorScheme.onSurfaceVariant,
                        )
                    }
                }
            }
            Spacer(Modifier.height(12.dp))
        }
        Text(
            state.achievementSummary,
            style = MaterialTheme.typography.bodyMedium,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )
        // The hardcore switch that used to sit here is gone: ARMSX's RetroAchievements support
        // is softcore only, so there is nothing for it to toggle (see
        // EmulationMenuViewModel.requestToggleHardcore). Showing an always-off switch would
        // just read as a bug.
    }
    // Inline unlock list — no need to open the full screen (it's still available via the
    // button above).
    state.achievements.forEach { item -> InGameAchievementRow(item) }
}

@Composable
private fun InGameAchievementRow(item: AchievementItem) {
    Surface(
        Modifier.fillMaxWidth(),
        shape = RoundedCornerShape(14.dp),
        color = if (item.unlocked) MaterialTheme.colorScheme.primaryContainer
        else MaterialTheme.colorScheme.surfaceVariant.copy(alpha = 0.42f),
        border = BorderStroke(
            1.dp,
            if (item.unlocked) MaterialTheme.colorScheme.primary.copy(alpha = 0.5f)
            else MaterialTheme.colorScheme.outline.copy(alpha = 0.3f),
        ),
    ) {
        Row(Modifier.padding(10.dp), verticalAlignment = Alignment.CenterVertically) {
            if (item.iconUrl.isNotBlank()) {
                AsyncImage(
                    item.iconUrl,
                    item.title,
                    Modifier.size(40.dp).clip(RoundedCornerShape(9.dp)),
                    contentScale = ContentScale.Crop,
                )
            } else {
                Box(
                    Modifier.size(40.dp).clip(RoundedCornerShape(9.dp))
                        .background(MaterialTheme.colorScheme.surface),
                    contentAlignment = Alignment.Center,
                ) { Text(if (item.unlocked) "★" else "☆") }
            }
            Spacer(Modifier.width(10.dp))
            Column(Modifier.weight(1f)) {
                Text(item.title, style = MaterialTheme.typography.titleSmall, fontWeight = FontWeight.SemiBold, maxLines = 1, overflow = TextOverflow.Ellipsis)
                Text(item.description, style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant, maxLines = 2, overflow = TextOverflow.Ellipsis)
                if (item.progress.isNotBlank()) {
                    Text(item.progress, style = MaterialTheme.typography.labelSmall, color = MaterialTheme.colorScheme.primary)
                }
            }
            Spacer(Modifier.width(8.dp))
            // Flag missables in-game — the actionable warning while you're actually playing.
            // Progression/Win badges are left to the full achievements screen to avoid clutter here.
            if (item.type == 1) {
                StatusChip(str("ra.typeChip.missable"), Color(0xFFF5A623))
                Spacer(Modifier.width(8.dp))
            }
            Text(
                "${item.points}",
                style = MaterialTheme.typography.labelMedium,
                color = if (item.unlocked) Success else MaterialTheme.colorScheme.onSurfaceVariant,
                fontWeight = FontWeight.Bold,
            )
        }
    }
}

@Composable
private fun HardcoreBadge() {
    // Firebrick red to match the old UI's hardcore pill (theme Danger reads pink here).
    Surface(shape = RoundedCornerShape(6.dp), color = Color(0xFFB22222)) {
        Text(
            "HC",
            modifier = Modifier.padding(horizontal = 7.dp, vertical = 2.dp),
            color = Color.White,
            style = MaterialTheme.typography.labelSmall,
            fontWeight = FontWeight.Bold,
        )
    }
}

private data class MenuAction(
    val title: String,
    val detail: String,
    val glyph: String,
    val accent: Color?,
    val action: () -> Unit,
)

@Composable
private fun ActionGrid(actions: List<MenuAction>, selected: Int, onSelect: (Int) -> Unit) {
    Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
        actions.forEachIndexed { index, item ->
            val active = index == selected
            Surface(
                onClick = { onSelect(index); item.action() },
                modifier = Modifier
                    .fillMaxWidth()
                    .controllerFocusable("pause.action.$index", onConfirm = { onSelect(index); item.action() }),
                shape = RoundedCornerShape(16.dp),
                color = if (active) MaterialTheme.colorScheme.primary.copy(alpha = 0.14f)
                else MaterialTheme.colorScheme.surfaceVariant.copy(alpha = 0.48f),
                border = BorderStroke(
                    1.dp,
                    if (active) MaterialTheme.colorScheme.primary.copy(alpha = 0.72f)
                    else MaterialTheme.colorScheme.outline.copy(alpha = 0.34f),
                ),
            ) {
                Row(Modifier.padding(horizontal = 13.dp, vertical = 11.dp), verticalAlignment = Alignment.CenterVertically) {
                    Text(
                        item.glyph,
                        color = item.accent ?: MaterialTheme.colorScheme.primary,
                        fontSize = 19.sp,
                        fontWeight = FontWeight.Bold,
                        modifier = Modifier.width(30.dp),
                    )
                    Column(Modifier.weight(1f)) {
                        Text(item.title, style = MaterialTheme.typography.titleSmall, fontWeight = FontWeight.SemiBold)
                        if (item.detail.isNotBlank()) {
                            Text(
                                item.detail,
                                style = MaterialTheme.typography.bodySmall,
                                color = MaterialTheme.colorScheme.onSurfaceVariant,
                                maxLines = 1,
                                overflow = TextOverflow.Ellipsis,
                            )
                        }
                    }
                }
            }
        }
    }
}

@Composable
private fun SectionCard(title: String, content: @Composable ColumnScope.() -> Unit) {
    Surface(
        modifier = Modifier.fillMaxWidth(),
        shape = RoundedCornerShape(17.dp),
        color = MaterialTheme.colorScheme.surfaceVariant.copy(alpha = 0.42f),
        border = BorderStroke(1.dp, MaterialTheme.colorScheme.outline.copy(alpha = 0.34f)),
    ) {
        Column(Modifier.padding(13.dp)) {
            Text(title, style = MaterialTheme.typography.titleSmall, fontWeight = FontWeight.Bold)
            Spacer(Modifier.height(8.dp))
            content()
        }
    }
}

@Composable
private fun <T> HorizontalOptions(
    title: String,
    options: List<Pair<T, String>>,
    selected: T,
    onSelect: (T) -> Unit,
) {
    SectionCard(title) {
        HorizontalOptionRow(options, selected, keyPrefix = title, onSelect = onSelect)
    }
}

// FramerateSlider / canonicalFramerate went with the NTSC and PAL rate rows they served: the
// PlayStation core takes its target rate from the disc's region and the Speed Limit percentage,
// and has no configurable vertical refresh to drag.

@Composable
private fun <T> HorizontalOptionRow(
    options: List<Pair<T, String>>,
    selected: T,
    keyPrefix: String,
    onSelect: (T) -> Unit,
) {
    Row(
        Modifier
            .fillMaxWidth()
            .bleedHorizontal(13.dp)
            .horizontalScroll(rememberScrollState())
            .padding(horizontal = 13.dp),
        horizontalArrangement = Arrangement.spacedBy(6.dp),
    ) {
        options.forEach { (value, label) ->
            OptionChip(label, selected == value, controllerId = "pause.$keyPrefix.$label") { onSelect(value) }
        }
    }
}

@Composable
private fun OptionChip(label: String, selected: Boolean, controllerId: String? = null, onClick: () -> Unit) {
    Surface(
        onClick = onClick,
        modifier = Modifier.controllerFocusable(controllerId, RoundedCornerShape(12.dp), onConfirm = onClick),
        shape = RoundedCornerShape(12.dp),
        color = if (selected) MaterialTheme.colorScheme.primary.copy(alpha = 0.17f)
        else MaterialTheme.colorScheme.surface,
        border = BorderStroke(
            1.dp,
            if (selected) MaterialTheme.colorScheme.primary else MaterialTheme.colorScheme.outline.copy(alpha = 0.38f),
        ),
    ) {
        Text(
            label,
            modifier = Modifier.padding(horizontal = 13.dp, vertical = 9.dp),
            color = if (selected) MaterialTheme.colorScheme.primary else MaterialTheme.colorScheme.onSurfaceVariant,
            style = MaterialTheme.typography.labelLarge,
            fontWeight = if (selected) FontWeight.Bold else FontWeight.Medium,
            maxLines = 2,
        )
    }
}

@Composable
private fun MenuSwitchRow(
    title: String,
    checked: Boolean,
    enabled: Boolean = true,
    description: String? = null,
    onCheckedChange: (Boolean) -> Unit,
) {
    Surface(
        onClick = { if (enabled) onCheckedChange(!checked) },
        modifier = Modifier
            .fillMaxWidth()
            .controllerFocusable(
                "pause.switch.$title",
                onConfirm = { if (enabled) onCheckedChange(!checked) },
                onLeft = { if (enabled) onCheckedChange(false) },
                onRight = { if (enabled) onCheckedChange(true) },
            ),
        enabled = enabled,
        shape = RoundedCornerShape(16.dp),
        color = MaterialTheme.colorScheme.surfaceVariant.copy(alpha = if (enabled) 0.42f else 0.24f),
        border = BorderStroke(1.dp, MaterialTheme.colorScheme.outline.copy(alpha = 0.34f)),
    ) {
        Row(
            Modifier.padding(horizontal = 13.dp, vertical = 9.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            Column(Modifier.weight(1f)) {
                Text(
                    title,
                    style = MaterialTheme.typography.titleSmall,
                    color = if (enabled) MaterialTheme.colorScheme.onSurface else MaterialTheme.colorScheme.onSurfaceVariant,
                    maxLines = 2,
                )
                if (description != null) {
                    Text(
                        description,
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                        maxLines = 3,
                    )
                }
            }
            Spacer(Modifier.width(10.dp))
            Switch(checked = checked, onCheckedChange = if (enabled) onCheckedChange else null)
        }
    }
}

/** Label + current value, cycled in place: tap/confirm and Right advance, Left steps back.
 *  The compact menu has no picker of its own and a segmented control doesn't fit its width,
 *  so multi-option settings cycle rather than expand. */
@Composable
private fun MenuCycleRow(
    title: String,
    valueLabel: String,
    onStep: (Int) -> Unit,
) {
    Surface(
        onClick = { onStep(1) },
        modifier = Modifier
            .fillMaxWidth()
            .controllerFocusable(
                "pause.cycle.$title",
                onConfirm = { onStep(1) },
                onLeft = { onStep(-1) },
                onRight = { onStep(1) },
            ),
        shape = RoundedCornerShape(16.dp),
        color = MaterialTheme.colorScheme.surfaceVariant.copy(alpha = 0.42f),
        border = BorderStroke(1.dp, MaterialTheme.colorScheme.outline.copy(alpha = 0.34f)),
    ) {
        Row(
            Modifier.padding(horizontal = 13.dp, vertical = 9.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            Text(
                title,
                modifier = Modifier.weight(1f),
                style = MaterialTheme.typography.titleSmall,
                color = MaterialTheme.colorScheme.onSurface,
                maxLines = 2,
            )
            Spacer(Modifier.width(10.dp))
            Text(
                valueLabel,
                style = MaterialTheme.typography.titleSmall,
                color = MaterialTheme.colorScheme.primary,
            )
        }
    }
}

@Composable
private fun CompactAction(title: String, glyph: String, modifier: Modifier, onClick: () -> Unit) {
    Surface(
        onClick = onClick,
        modifier = modifier.controllerFocusable("pause.compact.$title", onConfirm = onClick),
        shape = RoundedCornerShape(14.dp),
        color = MaterialTheme.colorScheme.surface,
        border = BorderStroke(1.dp, MaterialTheme.colorScheme.outline.copy(alpha = 0.42f)),
    ) {
        Row(
            Modifier.padding(horizontal = 12.dp, vertical = 11.dp),
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.spacedBy(8.dp),
        ) {
            Text(glyph, color = MaterialTheme.colorScheme.primary, fontWeight = FontWeight.Bold)
            Text(title, style = MaterialTheme.typography.labelLarge, maxLines = 2)
        }
    }
}

private fun Modifier.bleedHorizontal(edge: androidx.compose.ui.unit.Dp): Modifier = layout { measurable, constraints ->
    val edgePx = edge.roundToPx()
    val expandedMin = (constraints.minWidth + edgePx * 2).coerceAtMost(constraints.maxWidth + edgePx * 2)
    val expandedMax = constraints.maxWidth + edgePx * 2
    val placeable = measurable.measure(
        constraints.copy(
            minWidth = expandedMin,
            maxWidth = expandedMax,
        ),
    )
    layout(constraints.maxWidth, placeable.height) {
        placeable.placeRelative(-edgePx, 0)
    }
}
