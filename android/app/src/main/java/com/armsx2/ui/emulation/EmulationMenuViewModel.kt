package com.armsx2.ui.emulation

import android.app.Application
import androidx.lifecycle.AndroidViewModel
import com.armsx2.config.Settings
import com.armsx2.i18n.I18n
import com.armsx2.input.ControllerMappings
import com.armsx2.runtime.MainActivityRuntime
import com.armsx2.ui.InGameOverlay
import com.armsx2.ui.achievements.AchievementItem
import com.armsx2.ui.achievements.parseAchievementItems
import kr.co.iefriends.pcsx2.NativeApp

enum class EmulationMenuTab(val titleKey: String) {
    Session("games.info.inGameMenu.title"),
    Graphics("tab.renderer"),
    Fixes("tab.fixes"),
    Performance("tab.performance"),
    Controls("tab.controls"),
    Options("action.settings"),
    Achievements("ra.title"),
    // No Friends tab. It lived at the end of a rail that scrolls, so reaching it meant knowing it
    // was there and then hunting for it — it is a header button with its own overlay instead.
}

data class EmulationMenuUiState(
    val tab: EmulationMenuTab = EmulationMenuTab.Session,
    val selectedAction: Int = 0,
    val saveSlot: Int = 0,
    val settings: Settings = Settings(),
    val touchControlsVisible: Boolean = true,
    val rumbleEnabled: Boolean = true,
    val multitapEnabled: Boolean = false,
    // Rewind ([emulation] rewind). The Session pane only shows its row when this is on —
    // with rewind off there is no buffer, so a row could only ever report "nothing to
    // rewind to". rewindStepLabel is how far one press goes, derived from the configured
    // snapshot frequency.
    val rewindEnabled: Boolean = false,
    val rewindStepLabel: String = "0.5s",
    val hardcore: Boolean = false,
    // Non-null while the hardcore confirm dialog is up; holds the target state.
    val pendingHardcore: Boolean? = null,
    val achievementSummary: String = I18n.get("ra.status.noAchievements.title"),
    // RA account line for the pause-menu panel (empty / 0 when not logged in).
    val raUserName: String = "",
    val raScore: Long = 0,
    val raSoftcoreScore: Long = 0,
    val raAvatarUrl: String = "",
    val achievements: List<AchievementItem> = emptyList(),
    // RetroAchievements rich-presence line ("what you're doing right now"); shown in the
    // pause-menu header when a set is loaded. Empty when RA is off / no set.
    val richPresence: String = "",
    // Current boot ELF CRC — the value that goes in a <SERIAL>_<CRC>.pnach filename.
    val gameCRC: String = "",
)

class EmulationMenuViewModel(application: Application) : AndroidViewModel(application) {
    var state = androidx.compose.runtime.mutableStateOf(EmulationMenuUiState())
        private set

    var dismissHandler: (() -> Unit)? = null

    fun load(initialTab: EmulationMenuTab?) {
        val settings = InGameOverlay.settingsState.value
        // The PS1 core's own settings.toml, for the rows backed by Ps1Settings rather than
        // by the PS2-era Settings object above.
        val ps1 = runCatching {
            com.armsx2.config.Ps1SettingsStore.active(getApplication())
        }.getOrDefault(com.armsx2.config.Ps1Settings())
        // The native JSON emits the unlock list under "items"; count from that rather
        // than the non-existent "unlocked"/"total" keys the old code read (which always
        // fell through to rich presence). Fall back to rich presence when no set loaded.
        val raJson = runCatching { NativeApp.getAchievementsJSON().orEmpty() }.getOrDefault("")
        val items = runCatching { parseAchievementItems(raJson) }.getOrDefault(emptyList())
        val raRoot = runCatching { org.json.JSONObject(raJson) }.getOrNull()
        val richPresence = runCatching { NativeApp.getRichPresence().orEmpty() }.getOrDefault("")
        // Cheap: getGameCRC is a plain read of VMManager::GetCurrentCRC(), and with no VM it
        // reports 00000000 — which the filter below drops rather than showing as a real CRC.
        val gameCRC = runCatching { NativeApp.getGameCRC().orEmpty().trim().uppercase() }
            .getOrDefault("")
            .takeIf { it.matches(com.armsx2.DiscIdentity.CRC_PATTERN) && it != "00000000" }
            .orEmpty()
        val summary = if (items.isNotEmpty()) {
            "${items.count { it.unlocked }} / ${items.size}"
        } else {
            richPresence.ifBlank { I18n.get("ra.status.noAchievements.title") }
        }
        state.value = state.value.copy(
            tab = initialTab ?: state.value.tab,
            selectedAction = 0,
            saveSlot = MainActivityRuntime.currentSaveSlot.value,
            settings = settings,
            touchControlsVisible = com.armsx2.ui.touch.TouchControls.visible.value,
            rumbleEnabled = ControllerMappings.rumbleEnabled(),
            multitapEnabled = ControllerMappings.multitapEnabled(),
            rewindEnabled = ps1.rewind,
            rewindStepLabel = String.format(
                java.util.Locale.US, "%.2gs",
                1.0 / ps1.rewindFrequency.coerceAtLeast(1),
            ),
            hardcore = runCatching { NativeApp.isHardcoreMode() }.getOrDefault(false),
            achievementSummary = summary,
            raUserName = raRoot?.optString("userName").orEmpty(),
            raScore = (raRoot?.optLong("score") ?: 0L).coerceAtLeast(0),
            raSoftcoreScore = (raRoot?.optLong("softcoreScore") ?: 0L).coerceAtLeast(0),
            raAvatarUrl = raRoot?.optString("avatarUrl").orEmpty(),
            achievements = items,
            richPresence = richPresence,
            gameCRC = gameCRC,
        )
    }

    fun selectTab(tab: EmulationMenuTab) {
        // Nav tick when flipping to a different in-game menu tab (bumpers via cycleTab, or a tap).
        if (tab != state.value.tab) com.armsx2.MenuSfx.play(com.armsx2.MenuSfx.Event.NAV)
        state.value = state.value.copy(tab = tab, selectedAction = 0)
    }

    fun cycleTab(delta: Int) {
        val tabs = EmulationMenuTab.entries
        val current = tabs.indexOf(state.value.tab)
        selectTab(tabs[(current + delta).floorMod(tabs.size)])
    }

    fun moveSelection(delta: Int) {
        val max = actionCount(state.value.tab) - 1
        val before = state.value.selectedAction
        val next = (before + delta).coerceIn(0, max.coerceAtLeast(0))
        if (next != before) com.armsx2.MenuSfx.play(com.armsx2.MenuSfx.Event.NAV)
        state.value = state.value.copy(selectedAction = next)
    }

    fun selectAction(index: Int) {
        state.value = state.value.copy(selectedAction = index)
    }

    fun activateSelection() {
        when (state.value.tab) {
            // Index-for-index with SessionPane's action list. It used to skip Fast-Forward
            // entirely (0 resume, 1 restart, ...), so activating index 1 with the pad restarted
            // the game instead of fast-forwarding it.
            EmulationMenuTab.Session -> when (state.value.selectedAction) {
                0 -> resume()
                1 -> { MainActivityRuntime.instance?.toggleFastForward(); resume() }
                2 -> MainActivityRuntime.resetGame()
                3 -> MainActivityRuntime.promptSwapDisc()
                4 -> MainActivityRuntime.closeGame()
                5 -> {
                    MainActivityRuntime.instance?.let { com.armsx2.Screenshots.capture(it.applicationContext) }
                    com.armsx2.ui.GameOsd.toast("Screenshot saved")
                    resume()
                }
                // Only present when rewind is on, matching SessionPane's list. Deliberately
                // does NOT resume — repeated presses scrub further back.
                6 -> if (state.value.rewindEnabled) rewindStepBack()
            }
            // Graphics, Fixes and Performance are registry-driven panes — every control in them
            // registers with SettingsControllerNav and the router drives it directly — so there is
            // no discrete action grid to activate here. Graphics used to map indices 0-3 onto
            // setRenderer("auto"/"vulkan"/"opengl"/"software"), a four-entry picker that no longer
            // exists (one unified renderer list, backed by settings.toml), and Performance mapped
            // 0-2 onto PS2 Settings writes that never reached the core.
            EmulationMenuTab.Graphics -> Unit
            EmulationMenuTab.Fixes -> Unit
            EmulationMenuTab.Performance -> Unit
            EmulationMenuTab.Controls -> when (state.value.selectedAction) {
                0 -> editTouchControls()
                1 -> toggleTouchControls()
            }
            // The widescreen / no-interlacing entries are gone with their rows: both were PNACH
            // patch categories that only exist in the PS2 patch archive. So is "Enable Patches",
            // which wrote EmuCore/EnablePatches — a PCSX2 key nothing in this build reads.
            //
            // Index 0 is now the PS1 cheats master for the RUNNING game, matching the first
            // switch row in OptionsPane, and it goes through the same per-game layer the Cheats
            // screen writes rather than a field with no native side.
            EmulationMenuTab.Options -> when (state.value.selectedAction) {
                0 -> toggleCheatsForRunningGame()
                1 -> updateSettings { it.copy(enableFastBoot = !it.enableFastBoot) }
            }
            // One action left on this tab: the hardcore switch that used to be index 0 is gone
            // (softcore only), so opening the full screen is all there is.
            EmulationMenuTab.Achievements -> openAchievements()
        }
    }

    fun resume() {
        dismissHandler?.invoke() ?: resumeImmediately()
    }

    fun resumeImmediately() {
        InGameOverlay.toggle()
    }

    /**
     * Item 3: the in-game compact menu only exposes a reduced set of settings. This opens the
     * FULL settings (every category the compact menu omits — Video, Emulation, BIOS, Library,
     * Interface, Advanced) over the running game via the app-nav's showLibrary layer.
     * Changes live-apply through the settings system while the VM is running.
     */
    fun openFullSettings() = com.armsx2.ui.WindowImpl.openInGameScreen(com.armsx2.ui.InGameScreen.Settings)

    /** In-game access to the manager screens the library drawer exposes. */
    fun openMemcard() = com.armsx2.ui.WindowImpl.openInGameScreen(com.armsx2.ui.InGameScreen.Memcard)

    fun openPatches() = com.armsx2.ui.WindowImpl.openInGameScreen(com.armsx2.ui.InGameScreen.Patches)

    /**
     * Flip `[cheats] enabled` for the game that is running — the controller-nav twin of the
     * first switch row on the Options pane.
     *
     * Refuses while RetroAchievements hardcore is active, and refuses when nothing is running:
     * there is no global cheat list to toggle, by design.
     */
    private fun toggleCheatsForRunningGame() {
        if (state.value.hardcore) return
        val ctx = getApplication<Application>().applicationContext
        val gameKey = com.armsx2.config.Ps1SettingsStore.activeGameKey ?: return
        val current = com.armsx2.cheats.Ps1Cheats.settingsFor(ctx, gameKey)
        com.armsx2.cheats.Ps1Cheats.setEnabled(
            ctx,
            gameKey,
            !current.cheatsEnabled,
            current.cheatsEnabledCodes,
        )
    }

    fun openControlsManager() = com.armsx2.ui.WindowImpl.openInGameScreen(com.armsx2.ui.InGameScreen.Controls)

    fun openSkins() = com.armsx2.ui.WindowImpl.openInGameScreen(com.armsx2.ui.InGameScreen.Skins)

    fun saveState() {
        MainActivityRuntime.instance?.saveState()
    }

    fun loadState() {
        // Resume/dismiss only after the state has actually loaded (avoids the race
        // where the menu resumed the VM before the load landed).
        MainActivityRuntime.instance?.loadState { resume() }
    }

    fun previousSlot() = setSaveSlot((state.value.saveSlot - 1).floorMod(10))

    fun nextSlot() = setSaveSlot((state.value.saveSlot + 1) % 10)

    fun setSaveSlot(slot: Int) {
        val normalized = slot.coerceIn(0, 9)
        MainActivityRuntime.currentSaveSlot.value = normalized
        state.value = state.value.copy(saveSlot = normalized)
    }

    // setRenderer / setUpscale / setAspectRatio / setSpeed / setFpsLimit / setFrameSkip are gone
    // with the rows that called them. Every one wrote a PS2 Settings field and then poked a
    // NativeApp entry point with an empty body in this port — renderAuto / renderVulkan /
    // renderUpscalemultiplier / setAspectRatio / setFrameSkip — so the whole path persisted a
    // value the emulator never saw. The pause menu's Display and Performance panes now edit
    // Ps1Settings (settings.toml) directly and live-apply through Ps1Display / Ps1Pacing.
    //
    // setTextureFiltering / setBlending / setTexturePreloading / setHardwareDownloadMode and the
    // two EE cycle setters went earlier for the same reason, plus not describing PlayStation
    // hardware at all (the GS texture cache and blend unit, GS readback, the Emotion Engine's clock).

    fun setVolume(value: Int) = updateSettings { it.copy(audioVolume = value.coerceIn(0, 200)) }

    fun setAudioBuffer(value: Int) = updateSettings { it.copy(audioBufferMs = value.coerceIn(10, 200)) }

    /** Universal on-screen-display toggle (old-UI style): flips the perf stats as a
     *  group; the granular per-stat toggles stay in All Settings. Notifications are
     *  left alone so achievement/message popups aren't affected. */
    // The one-tap OSD master mirrors what refresh showed by default: FPS/VPS, speed,
    // the EE/GS/VU/GPU perf lines, resolution, GS + GPU pipeline stats, frame times,
    // the hardware-info line, and the "ARMSX2 <version>" banner. Granular control of
    // each stays in All Settings; the bottom Settings-summary / Inputs overlays are
    // left out so this can't force those debug strips on.
    fun setOsdMaster(enabled: Boolean) = updateSettings {
        it.copy(
            osdShowFps = enabled,
            osdShowVps = enabled,
            osdShowSpeed = enabled,
            osdShowCpu = enabled,
            osdShowGpu = enabled,
            osdShowResolution = enabled,
            osdShowGsStats = enabled,
            osdShowFrameTimes = enabled,
            osdShowHardwareInfo = enabled,
            osdShowHostUsage = enabled,
            osdShowGpuStats = enabled,
            osdShowVersion = enabled,
        )
    }

    /** Simple OSD: just the FPS/VPS counter, none of the verbose CPU/GPU/GS/frame-time/
     *  hardware lines. Mutually exclusive with the full OSD through the shared osdShow*
     *  fields — the menu reads "FPS on + everything-else off" as the simple state. */
    fun setOsdSimple(enabled: Boolean) = updateSettings {
        it.copy(
            osdShowFps = enabled,
            osdShowVps = false,
            osdShowSpeed = false,
            osdShowCpu = false,
            osdShowGpu = false,
            osdShowResolution = false,
            osdShowGsStats = false,
            osdShowFrameTimes = false,
            osdShowHardwareInfo = false,
            osdShowHostUsage = false,
            osdShowGpuStats = false,
            osdShowVersion = false,
        )
    }

    fun setRumble(enabled: Boolean) {
        ControllerMappings.setRumbleEnabled(enabled)
        NativeApp.sRumbleEnabled = enabled
        state.value = state.value.copy(rumbleEnabled = enabled)
    }

    fun setMultitap(enabled: Boolean) {
        // Real now: persists [input] multitap, pushes NativeApp.setMultitapEnabled into the
        // running core, and re-caps PadRouter so players 2-4 can claim a slot. It used to be
        // a no-op stub behind a live-looking switch.
        ControllerMappings.setMultitapEnabled(enabled)
        state.value = state.value.copy(multitapEnabled = enabled)
    }

    /** One rewind step, from the Session pane's row. Stays in the menu so it can be pressed
     *  repeatedly; the hold-to-rewind hotkey is the fluent way to do the same thing. */
    fun rewindStepBack() {
        com.armsx2.core.Ps1Emulation.stepBack()
    }

    fun editTouchControls() {
        dismissHandler = null
        InGameOverlay.editTouchLayout()
    }

    fun toggleTouchControls() {
        val enabled = !state.value.touchControlsVisible
        com.armsx2.ui.touch.TouchControls.visible.value = enabled
        state.value = state.value.copy(touchControlsVisible = enabled)
    }

    // SOFTCORE ONLY. ARMSX's RetroAchievements support does not have a hardcore path yet — the
    // emulator needs a lot more testing before an unlock from it may claim hardcore credit on
    // someone's RA account — so this is inert and the row that used to call it is hidden. The
    // native side refuses hardcore too (NativeApp.setHardcoreMode is a logged no-op); this is
    // just the layer that keeps the confirm dialog from ever appearing.
    fun requestToggleHardcore() {
        // Intentionally does nothing.
    }

    fun confirmToggleHardcore() {
        val target = state.value.pendingHardcore ?: return
        NativeApp.setHardcoreMode(target)
        state.value = state.value.copy(hardcore = target, pendingHardcore = null)
        // Enabling hardcore only takes hold on a system reset, and the VM is paused
        // behind this menu — so the native "will be enabled on system reset" toast
        // just sits there. Reboot now so "Enable & restart" actually restarts.
        // (Disabling stays live — casual mode applies immediately.)
        if (target) MainActivityRuntime.restart()
    }

    fun cancelToggleHardcore() {
        state.value = state.value.copy(pendingHardcore = null)
    }

    /** Open the full RetroAchievements screen (list + options) over the paused game. */
    fun openAchievements() = com.armsx2.ui.WindowImpl.openInGameScreen(com.armsx2.ui.InGameScreen.Achievements)

    fun updateSettings(transform: (Settings) -> Settings) {
        // ★ Transform the LIVE shared settings, not this screen's snapshot. state.value.settings is
        // only refreshed in load(), so every write here shipped the whole Settings object as it
        // looked when the menu opened — silently reverting anything changed elsewhere since. That
        // is the long-standing whole-object clobber, and it is why the FPS cap read back as 0
        // moments after being set: a later save from a stale snapshot re-pushed the old value.
        val updated = transform(InGameOverlay.settingsState.value)
        InGameOverlay.saveSettings(updated)
        state.value = state.value.copy(settings = updated)
    }

    private fun actionCount(tab: EmulationMenuTab): Int = when (tab) {
        // MUST match SessionPane's action list length. This was 4 against a list of 5, so the pad
        // could never reach Close at all. Now 6 (Resume, Fast-Forward, Restart, Swap Disc, Close,
        // Screenshot), plus Rewind when [emulation] rewind is on — which is exactly when
        // SessionPane appends its row.
        EmulationMenuTab.Session -> if (state.value.rewindEnabled) 7 else 6
        // 0 for the three registry-driven panes: their rows are SettingsControllerNav items, not
        // indices into an action grid, so a non-zero count here would move a selection nothing
        // draws. Graphics was 4 against the old four-entry renderer picker.
        EmulationMenuTab.Graphics -> 0
        EmulationMenuTab.Fixes -> 0
        EmulationMenuTab.Performance -> 0
        EmulationMenuTab.Controls -> 2
        // 2, not 3: "Enable Patches" is gone with the PS2 patch machinery, leaving the PS1
        // cheats master and Skip BIOS. A stale 3 would let the pad select an index no row
        // draws and whose activate case does nothing — a dead stop the user reads as the
        // button being broken.
        EmulationMenuTab.Options -> 2
        // 1, not 2: the hardcore switch was removed (softcore only), leaving just "open the
        // achievements screen".
        EmulationMenuTab.Achievements -> 1
    }

    private fun Int.floorMod(modulus: Int): Int = ((this % modulus) + modulus) % modulus
}

object EmulationMenuInputController {
    private var owner: EmulationMenuViewModel? = null
    private var pendingTab: EmulationMenuTab? = null

    // Two-zone nav. The pause menu is a vertical TAB column on the left and a
    // CONTENT pane on the right. `inContent` = false means the D-pad walks the tab
    // column (Up/Down between tabs, which switches the shown pane); Right (or A)
    // steps into the content pane, where every control is a SettingsControllerNav
    // registry item and the router drives it (Up/Down move, Left/Right adjust, A
    // confirm). B (or Left off the first control) returns to the tab column.
    val inContent = androidx.compose.runtime.mutableStateOf(false)
    private val nav get() = com.armsx2.ui.settings.SettingsControllerNav

    // Set by a modal panel drawn OVER the menu (Friends) for as long as it is open; the lambda
    // closes it.
    //
    // Without this the pad kept driving the menu underneath: move() falls through to the tab
    // column whenever inContent is false, so the D-pad walked tabs behind the panel and the
    // panel's own buttons — which are in the same registry — could never be reached. The overlay
    // is on top visually, so it has to be on top for input too.
    var overlayDismiss: (() -> Unit)? = null

    fun bind(viewModel: EmulationMenuViewModel) {
        owner = viewModel
        viewModel.load(pendingTab)
        pendingTab = null
        inContent.value = false
    }

    fun unbind(viewModel: EmulationMenuViewModel) {
        if (owner === viewModel) owner = null
    }

    fun open(tab: EmulationMenuTab = EmulationMenuTab.Session) {
        pendingTab = tab
        if (!com.armsx2.ui.WindowImpl.overlayVisible.value) InGameOverlay.open()
        owner?.selectTab(tab)
        inContent.value = false
    }

    private fun enterContent() {
        inContent.value = true
        nav.clearSelection()
        nav.move(1) // select the first content control so the highlight appears
    }

    private fun exitContent() {
        inContent.value = false
        nav.clearSelection()
    }

    fun move(dx: Int, dy: Int): Boolean {
        // A panel is over the menu: everything is registry nav, there is no tab column to walk.
        if (overlayDismiss != null) {
            when {
                dy != 0 -> nav.moveSpatial(0, dy)
                dx != 0 -> if (!nav.adjust(dx)) nav.moveSpatial(dx, 0)
            }
            return true
        }
        val viewModel = owner ?: return false
        if (!inContent.value) {
            // Tab column (vertical): Up/Down switch tabs; Right steps into content.
            when {
                dy < 0 -> viewModel.cycleTab(-1)
                dy > 0 -> viewModel.cycleTab(1)
                dx > 0 -> enterContent()
            }
            return true
        }
        // Content pane: registry-driven.
        when {
            dy != 0 -> nav.moveSpatial(0, dy)
            dx < 0 -> if (!nav.adjust(-1) && !nav.moveSpatial(-1, 0)) exitContent()
            dx > 0 -> if (!nav.adjust(1)) nav.moveSpatial(1, 0)
        }
        return true
    }

    /** L1 / R1 always cycle tabs, snapping back to the tab column. */
    fun tab(delta: Int): Boolean {
        // Swallowed while a panel is up: the tabs are behind it, and silently switching the pane
        // you cannot see is worse than doing nothing.
        if (overlayDismiss != null) return true
        val viewModel = owner ?: return false
        if (inContent.value) exitContent()
        viewModel.cycleTab(delta)
        return true
    }

    fun confirm(): Boolean {
        if (overlayDismiss != null) { nav.confirm(); return true }
        owner ?: return false
        if (!inContent.value) { enterContent(); return true }
        nav.confirm()
        return true
    }

    fun back(): Boolean {
        // Back closes the panel, not the menu behind it.
        overlayDismiss?.let { dismiss -> dismiss(); return true }
        if (inContent.value) { exitContent(); return true }
        owner?.resume() ?: return false
        return true
    }
}
