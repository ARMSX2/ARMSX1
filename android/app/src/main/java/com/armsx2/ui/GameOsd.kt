package com.armsx2.ui

import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.key
import androidx.compose.runtime.mutableFloatStateOf
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateListOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.Shadow
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.armsx2.EmuState
import com.armsx2.config.ConfigStore
import com.armsx2.config.Settings
import com.armsx2.runtime.MainActivityRuntime
import com.armsx2.ui.settings.OSD_COLORS
import kotlinx.coroutines.delay
import kr.co.iefriends.pcsx2.NativeApp
import java.util.Locale
import kotlin.math.roundToInt

/**
 * The in-game on-screen display.
 *
 * The PS1 core draws no OSD of its own: PCSX2's was rendered by the GS through imgui/FSUI, and both
 * were cut when the front-end moved to Jetpack Compose. Now that the core runs IN-PROCESS behind a
 * Compose-hosted SurfaceView, the OSD belongs on the Compose side — it costs the emulation thread
 * nothing, it scales with the user's UI Size, and it can show host-side facts (fast-forward,
 * paused, "screenshot saved") that the core never knew about in the first place.
 *
 * Two parts:
 *  - a persistent stats block (top-left), gated by the OSD settings that already exist —
 *    [InGameOverlay.osdMode] plus the per-stat `Settings.osdShow*` flags the On-Screen settings tab
 *    writes. No new preference is invented here; the inherited PS2-shaped flags are re-pointed at
 *    the PS1 equivalents (osdShowCpu -> R3000A, osdShowGpu -> the rasteriser, osdShowGsStats -> the
 *    SPU/MDEC/CDROM/DMA/GTE breakdown, osdShowHardwareInfo -> the active renderer).
 *  - a transient note area (top-centre) for one-shot events: paused, fast-forward on/off, screenshot
 *    saved, state saved. Those used to be Android Toasts, which render OUTSIDE the game surface and
 *    read as a system message rather than part of the emulator.
 *
 * FPS and speed come from the core itself: `ArmsxApp::publishSessionTelemetry()` averages the rate
 * frames are actually emulated at over a half-second window and publishes it (plus the game's
 * nominal rate) as atomics, which `NativeApp.getFPS()` / `getNominalFrameRate()` read. Both were
 * inert PS2 stubs returning 0 until that landed, which is why this block had nothing to show. The
 * row still reads "--" rather than inventing a number whenever the core reports 0 (no game, or
 * paused): the display refresh rate and a Compose frame count are both easy to reach for here and
 * neither of them is the emulated frame rate.
 */
object GameOsd {
    private const val DEFAULT_NOTE_MS = 1_800L
    private const val MAX_NOTES = 3

    /** Mirrors the VM's real paused state (set either side of the JNI pause/resume). */
    val paused = mutableStateOf(false)

    /** Mirrors the core's fast-forward flag (`ArmsxSession::setFastForwardEnabled`). */
    val fastForward = mutableStateOf(false)

    /** Emulated frames per second, or a non-positive value when the core exposes no counter. */
    val fps = mutableFloatStateOf(-1f)

    /** The game's nominal rate (~59.94 NTSC / 50 PAL). Paired with [fps] it turns the speed row
     *  into a real percentage instead of a fixed "1×" that only ever restated the FF multiplier. */
    val nominalFps = mutableFloatStateOf(0f)

    /** The multiplier fast-forward actually runs at, as the core has it — `[runtime]
     *  fast_forward_speed`, pushed through `NativeApp.setSpeedLimits`. Refreshed by
     *  [setFastForward] (and by the pause menu's speed row) rather than read per frame: the
     *  value only changes from the settings UI, which always writes before the next toggle.
     *  Kept as a label ("2×" / "Unlimited") so nothing can claim a speed the core is not
     *  running, and as Compose state so the menu row re-reads it when it changes. */
    private val fastForwardLabelState = mutableStateOf("2×")
    var fastForwardLabel: String
        get() = fastForwardLabelState.value
        set(value) {
            fastForwardLabelState.value = value
        }

    /**
     * Indices into [stats]. MUST stay in lockstep with `PSXE_HOST_STAT_*` in
     * frontend/host_stats.h — the native side writes this array by ordinal, so an index
     * inserted on one side and not the other silently relabels every row below it.
     *
     * Every entry is a real measurement, per emulated frame, averaged over the core's last
     * half-second window (WORST_FRAME_MS excepted — averaging a spike away is exactly what
     * hides a stutter). Nothing here is modelled: see the note on measurement in psx/perf.h.
     */
    object Stat {
        const val FRAME_MS = 0
        const val EMU_MS = 1
        const val PRESENT_MS = 2
        const val IDLE_MS = 3
        const val WORST_FRAME_MS = 4
        const val CPU_INSTRUCTIONS = 5
        const val CPU_CYCLES = 6
        const val GTE_OPS = 7
        const val GPU_TRIANGLES = 8
        const val GPU_RECTS = 9
        const val GPU_LINES = 10
        const val GPU_PIXELS = 11
        const val GPU_VRAM_WORDS = 12
        const val SPU_VOICES = 13
        const val MDEC_MACROBLOCKS = 14
        const val MDEC_BLOCKS = 15
        const val CDROM_SECTORS = 16
        const val DMA_WORDS = 17
        const val DMA_GPU_WORDS = 18
        const val DMA_SPU_WORDS = 19
        const val DMA_MDEC_WORDS = 20
        const val DMA_CDROM_WORDS = 21
        const val DMA_OTC_WORDS = 22
        const val WIDTH = 23
        const val HEIGHT = 24

        // HOST (device) usage — the phone, not the emulated machine. Ordinals MUST match
        // frontend/host_stats.h exactly; that header is append-only for this reason.
        // A NEGATIVE value means unavailable and renders as "n/a" (see host_usage.h).
        const val HOST_CPU_PERCENT = 25
        const val HOST_CPU_CORES = 26
        const val HOST_RAM_MB = 27
        const val HOST_RAM_AVAILABLE_MB = 28
        const val HOST_GPU_PERCENT = 29
        const val INTERNAL_SCALE = 30
        const val COUNT = 31
    }

    /** The live snapshot. Reused rather than reallocated — `NativeApp.getStatistics` refills it
     *  in place, so polling costs no garbage. Read [statsVersion] to make a composable observe
     *  it: a plain array is not a Compose state and mutating it recomposes nothing. */
    val stats = DoubleArray(Stat.COUNT)
    val statsVersion = mutableIntStateOf(0)

    /** True once the core has published at least one window. Until then the rows read "--"
     *  rather than a screenful of zeroes. */
    val statsReady = mutableStateOf(false)

    /** The presentation backend that actually survived the fallback ladder — NOT the one the
     *  settings asked for. Read once when the VM comes up (it cannot change within a session)
     *  rather than polled. Empty when the core does not export it. */
    val renderer = mutableStateOf("")

    data class Note(val id: Long, val text: String, val expiresAt: Long)

    val notes = mutableStateListOf<Note>()
    private var nextNoteId = 1L

    /** Show a transient note over the game. Safe to call from any thread. */
    fun toast(text: String, durationMs: Long = DEFAULT_NOTE_MS) {
        if (text.isBlank()) return
        val activity = MainActivityRuntime.instance
        val push = Runnable {
            // One line per message: a rapid FF on/off/on replaces rather than stacks.
            notes.removeAll { it.text == text }
            notes.add(Note(nextNoteId++, text, System.currentTimeMillis() + durationMs))
            while (notes.size > MAX_NOTES) notes.removeAt(0)
        }
        if (activity != null) activity.runOnUiThread(push) else push.run()
    }

    fun setPaused(value: Boolean) {
        if (paused.value == value) return
        paused.value = value
        if (value) toast("Paused")
    }

    fun setFastForward(value: Boolean, speedLabel: String = fastForwardLabel) {
        fastForwardLabel = speedLabel
        if (fastForward.value == value) return
        fastForward.value = value
        toast(if (value) "Fast-forward ON ($speedLabel)" else "Fast-forward OFF")
    }

    /** Refill [stats] from the core. Returns false while the core has nothing to report (the
     *  overlay was only just armed, or the game is paused). */
    fun pollStats(): Boolean {
        val written = runCatching { NativeApp.getStatistics(stats) }.getOrDefault(0)
        if (written <= 0) return false
        statsVersion.intValue++
        statsReady.value = true
        return true
    }

    /**
     * The active renderer, resolved through reflection.
     *
     * `NativeApp.getActiveRenderer()` is owned by the renderer work, not by this file, so it is
     * called by name instead of linked against: a build where it has not landed yet degrades to
     * an empty string (and therefore no row) rather than failing to compile or throwing. Resolved
     * once per session and cached — the backend is fixed for the life of a VM.
     */
    fun refreshRenderer() {
        renderer.value = runCatching {
            val method = NativeApp::class.java.getMethod("getActiveRenderer")
            (method.invoke(null) as? String).orEmpty()
        }.getOrDefault("")
    }

    /** Drop every piece of session state — called when a game closes. */
    fun reset() {
        paused.value = false
        fastForward.value = false
        fps.floatValue = -1f
        nominalFps.floatValue = 0f
        renderer.value = ""
        statsReady.value = false
        stats.fill(0.0)
        // Also disarms the counters inside the core; leaving them live across a closed game
        // would instrument the next boot for a panel nobody asked for.
        runCatching { NativeApp.setStatisticsEnabled(false) }
        notes.clear()
    }
}

/** Thousands/millions shorthand, so a statistics row stays one line at OSD scale. Below 1000 the
 *  exact figure is kept — "3 sec" and "0 mb" are the readings that matter down there. */
private fun compact(value: Double): String = when {
    value >= 1_000_000.0 -> String.format(Locale.US, "%.2fM", value / 1_000_000.0)
    value >= 1_000.0 -> String.format(Locale.US, "%.1fK", value / 1_000.0)
    value >= 10.0 -> value.roundToInt().toString()
    value > 0.0 -> String.format(Locale.US, "%.1f", value)
    else -> "0"
}

/**
 * Draws [GameOsd] over the game surface.
 *
 * Hosted from [WindowImpl.Window] BELOW the pause menu and the manager screens (so those cover it)
 * and ABOVE the touch controls (so the notes stay readable). It is a plain [Box] with no gesture
 * modifiers, so it is invisible to the touch dispatcher — presses fall straight through to the
 * on-screen controls underneath. It inherits the `ScaledUi` density from [WindowImpl.Window], so it
 * follows the user's UI Size / UI Font Size like the rest of the Compose chrome.
 */
@Composable
fun GameOsdOverlay() {
    val state = MainActivityRuntime.eState.value
    val inGame = state == EmuState.RUNNING || state == EmuState.PAUSED
    if (!inGame || WindowImpl.showLibrary.value) return

    // Resolved per-game settings, NOT InGameOverlay.settingsState: that is only populated when the
    // pause menu opens, so at boot it still holds the previous game's values (the same trap
    // EmulationSurface.applyOutputScale documents). Re-resolved when the game changes and whenever
    // the pause menu closes, which is when the user can have edited an OSD toggle.
    val settingsKey = MainActivityRuntime.currentGame.value?.settingsKey
    val overlayOpen = WindowImpl.overlayVisible.value
    var settings by remember { mutableStateOf(Settings()) }
    LaunchedEffect(settingsKey, overlayOpen) {
        settings = runCatching { ConfigStore.resolveForGame(settingsKey) }.getOrDefault(Settings())
    }

    val mode = InGameOverlay.osdMode.value
    val showFps = when (mode) {
        InGameOverlay.OsdMode.Full, InGameOverlay.OsdMode.Min -> true
        InGameOverlay.OsdMode.Custom -> settings.osdShowFps
        InGameOverlay.OsdMode.Off -> false
    }
    val showSpeed = when (mode) {
        InGameOverlay.OsdMode.Full, InGameOverlay.OsdMode.Min -> true
        InGameOverlay.OsdMode.Custom -> settings.osdShowSpeed
        InGameOverlay.OsdMode.Off -> false
    }
    val showNotes = settings.osdShowMessages

    // Statistics block. The inherited PS2 flags are re-pointed at what this machine actually has;
    // Full/Min deliberately do NOT switch the heavy per-subsystem rows on, so the quick OSD cycle
    // hotkey can never silently arm the core's counters — that stays an explicit opt-in.
    val custom = mode == InGameOverlay.OsdMode.Custom
    val showCpu = custom && settings.osdShowCpu
    val showGpu = custom && settings.osdShowGpu
    val showDevices = custom && settings.osdShowGsStats
    val showFrameTimes = custom && settings.osdShowFrameTimes
    val showResolution = custom && settings.osdShowResolution
    val showRenderer = custom && settings.osdShowHardwareInfo
    val showHostUsage = custom && settings.osdShowHostUsage

    // Poll the core for the real rates. Cheap (2 Hz, two non-blocking atomic reads) and gated on
    // BOTH rows — the speed row is a percentage of the measured rate, so polling only for the FPS
    // row would leave "Emulation speed" alone on screen showing nothing. Stopped while paused: a
    // paused core reports 0 and the block already says PAUSED.
    val pausedNow = GameOsd.paused.value
    val needsRates = showFps || showSpeed
    LaunchedEffect(inGame, needsRates, pausedNow) {
        if (!inGame || !needsRates || pausedNow) return@LaunchedEffect
        while (true) {
            GameOsd.fps.floatValue = runCatching { NativeApp.getFPS() }.getOrDefault(0f)
            GameOsd.nominalFps.floatValue = runCatching { NativeApp.getNominalFrameRate() }.getOrDefault(0f)
            delay(500)
        }
    }

    // The statistics rows, and — more importantly — the core-side counters that feed them. This
    // is the gate the task calls for: with every statistics row off, setStatisticsEnabled(false)
    // strips the instrumentation out of the emulator entirely (psx/perf.h), so the cost is not
    // merely hidden. onDispose covers leaving the game with the block still switched on.
    val needsStats = showCpu || showGpu || showDevices || showFrameTimes || showResolution || showHostUsage
    DisposableEffect(inGame, needsStats) {
        runCatching { NativeApp.setStatisticsEnabled(inGame && needsStats) }
        onDispose { runCatching { NativeApp.setStatisticsEnabled(false) } }
    }
    LaunchedEffect(inGame, needsStats, pausedNow) {
        if (!inGame || !needsStats || pausedNow) return@LaunchedEffect
        while (true) {
            // ★ Re-ASSERT the arm rather than trusting the DisposableEffect above to have been
            // the last word on it. That effect is edge-triggered on (inGame, needsStats), and
            // GameOsd.reset() disarms the counters on every VM stop — including the stop half of
            // a restart. A restart whose STOPPED -> RUNNING pair lands inside one recomposition
            // changes neither key, so the edge never fires, and every row then reads a permanent
            // 0 (the disable also zeroes the native snapshot) while the game plays normally.
            // Free to repeat: psxe_host_set_stats_enabled() returns immediately when the state
            // is already what was asked for, so this does not touch the live counters.
            runCatching { NativeApp.setStatisticsEnabled(true) }
            GameOsd.pollStats()
            delay(500)
        }
    }

    // The active backend is fixed for the life of a VM, so this is read once per session instead
    // of polled. Keyed on the game as well so a restart into a different renderer re-reads it.
    LaunchedEffect(inGame, settingsKey, showRenderer) {
        if (inGame && showRenderer && GameOsd.renderer.value.isEmpty()) GameOsd.refreshRenderer()
    }

    // Expire notes. One ticker for the whole list rather than a coroutine per note.
    LaunchedEffect(GameOsd.notes.size) {
        while (GameOsd.notes.isNotEmpty()) {
            delay(100)
            val now = System.currentTimeMillis()
            GameOsd.notes.removeAll { it.expiresAt <= now }
        }
    }

    // osdScale is the existing On-Screen "OSD size" percent (default 65).
    //
    // osdColor holds a COLOUR VALUE from the OSD_COLORS palette, not an index into it — the
    // settings row writes `osdColor = OSD_COLORS[next]`. This read used to do
    // `OSD_COLORS.getOrNull(settings.osdColor)`, i.e. treat the value as an index, so any
    // non-default pick (0x66FF66 green, say) indexed far out of bounds, returned null and fell
    // back to white. That is why choosing an OSD colour appeared to do nothing.
    //
    // 0 means "unset" and is deliberately white: the palette's own first entry is 0x000000, which
    // as a real colour would be black-on-black over most games.
    val fontSize = (settings.osdScale.coerceIn(30, 200) * 0.22f).sp
    val tint = settings.osdColor.takeIf { it != 0 }
        ?.let { Color(0xFF000000.toInt() or (it and 0xFFFFFF)) } ?: Color.White
    val textStyle = TextStyle(
        color = tint,
        fontSize = fontSize,
        fontWeight = FontWeight.SemiBold,
        fontFamily = FontFamily.Monospace,
        // The scene behind is arbitrary; an unshadowed glyph vanishes over bright artwork.
        shadow = Shadow(Color.Black, Offset(1.5f, 1.5f), 3f),
    )

    val statRows = buildList {
        if (showFps) {
            val value = GameOsd.fps.floatValue
            add("FPS " + if (value > 0f) String.format(Locale.US, "%.1f", value) else "--")
        }
        if (showSpeed) {
            val measured = GameOsd.fps.floatValue
            val nominal = GameOsd.nominalFps.floatValue
            add(
                when {
                    GameOsd.paused.value -> "PAUSED"
                    // Real speed against the game's own nominal rate, so a device that cannot hold
                    // full speed reads honestly instead of always claiming "1×". The FF marker is
                    // kept alongside it: at 100% with fast-forward armed the player still wants to
                    // see that the toggle is on and the device simply cannot go faster.
                    measured > 0f && nominal > 0f -> {
                        val percent = ((measured / nominal) * 100f).roundToInt()
                        if (GameOsd.fastForward.value) "$percent% FF" else "$percent%"
                    }
                    GameOsd.fastForward.value -> "FF ${GameOsd.fastForwardLabel}"
                    else -> "1×"
                },
            )
        }

        // Statistics. Reading statsVersion is what subscribes this block to the snapshot —
        // GameOsd.stats is a plain array, so refilling it in place recomposes nothing on its own.
        val s = GameOsd.stats
        val ready = GameOsd.statsReady.value && GameOsd.statsVersion.intValue > 0

        if (showCpu) {
            add(
                if (!ready) "R3000A --"
                else {
                    val ins = s[GameOsd.Stat.CPU_INSTRUCTIONS]
                    val cyc = s[GameOsd.Stat.CPU_CYCLES]
                    val cpi = if (ins > 0.0) cyc / ins else 0.0
                    "R3000A ${compact(ins)} ins  ${compact(cyc)} cyc  " +
                        String.format(Locale.US, "%.2f CPI", cpi) +
                        "  GTE ${compact(s[GameOsd.Stat.GTE_OPS])}"
                },
            )
        }
        if (showGpu) {
            add(
                if (!ready) "GPU --"
                else "GPU ${compact(s[GameOsd.Stat.GPU_TRIANGLES])} tri  " +
                    "${compact(s[GameOsd.Stat.GPU_RECTS])} rect  " +
                    "${compact(s[GameOsd.Stat.GPU_LINES])} line  " +
                    "${compact(s[GameOsd.Stat.GPU_PIXELS])} px  " +
                    "${compact(s[GameOsd.Stat.GPU_VRAM_WORDS])} vram",
            )
        }
        if (showHostUsage) {
            // The DEVICE, not the PS1. A negative reading means the figure could not be read at
            // all and prints "n/a" — notably GPU busy, whose sysfs nodes are root-only on most
            // retail Android. Never render a negative as a number.
            add(
                if (!ready) "HOST --"
                else {
                    val cpu = s[GameOsd.Stat.HOST_CPU_PERCENT]
                    val ram = s[GameOsd.Stat.HOST_RAM_MB]
                    val avail = s[GameOsd.Stat.HOST_RAM_AVAILABLE_MB]
                    val gpu = s[GameOsd.Stat.HOST_GPU_PERCENT]
                    buildString {
                        append("HOST CPU ")
                        // 0..100 of the WHOLE device, same scale as the GPU figure beside it.
                        append(
                            if (cpu >= 0.0) String.format(Locale.US, "%.0f%%", cpu) else "n/a",
                        )
                        append("  GPU ")
                        append(
                            if (gpu >= 0.0) String.format(Locale.US, "%.0f%%", gpu) else "n/a",
                        )
                        append("  RAM ")
                        append(
                            if (ram >= 0.0) String.format(Locale.US, "%.0f MB", ram) else "n/a",
                        )
                        if (avail >= 0.0) {
                            append(String.format(Locale.US, " (%.0f free)", avail))
                        }
                    }
                },
            )
        }
        if (showDevices) {
            add(
                if (!ready) "SPU --"
                else String.format(Locale.US, "SPU %.1f voices", s[GameOsd.Stat.SPU_VOICES]) +
                    "  MDEC ${compact(s[GameOsd.Stat.MDEC_MACROBLOCKS])} mb / " +
                    "${compact(s[GameOsd.Stat.MDEC_BLOCKS])} blk" +
                    "  CD ${compact(s[GameOsd.Stat.CDROM_SECTORS])} sec",
            )
            add(
                if (!ready) "DMA --"
                else "DMA ${compact(s[GameOsd.Stat.DMA_WORDS])} wd  " +
                    "(gpu ${compact(s[GameOsd.Stat.DMA_GPU_WORDS])}" +
                    "  spu ${compact(s[GameOsd.Stat.DMA_SPU_WORDS])}" +
                    "  mdec ${compact(s[GameOsd.Stat.DMA_MDEC_WORDS])}" +
                    "  cd ${compact(s[GameOsd.Stat.DMA_CDROM_WORDS])}" +
                    "  otc ${compact(s[GameOsd.Stat.DMA_OTC_WORDS])})",
            )
        }
        if (showFrameTimes) {
            add(
                if (!ready) "FRAME --"
                else String.format(
                    Locale.US,
                    // emu = the whole guest frame (R3000A + rasteriser + devices, which this core
                    // interleaves and cannot separate by time); present = upload + swap; idle =
                    // what the frame limiter slept. idle at 0 with emu high is CPU-bound.
                    "FRAME %.1fms  emu %.1f  present %.1f  idle %.1f  worst %.1f",
                    s[GameOsd.Stat.FRAME_MS],
                    s[GameOsd.Stat.EMU_MS],
                    s[GameOsd.Stat.PRESENT_MS],
                    s[GameOsd.Stat.IDLE_MS],
                    s[GameOsd.Stat.WORST_FRAME_MS],
                ),
            )
        }
        if (showResolution) {
            val w = s[GameOsd.Stat.WIDTH].toInt()
            val h = s[GameOsd.Stat.HEIGHT].toInt()
            // WIDTH/HEIGHT are the EMULATED display size and never change when upscaling — the
            // console still thinks it is drawing 512x240. Showing only those made the OSD report
            // native resolution at every scale, so there was no way to confirm upscaling was on.
            // The scale is read from the live rasterizer, so a fallback to native shows as 1x.
            val scale = s[GameOsd.Stat.INTERNAL_SCALE].toInt()
            add(
                when {
                    !ready || w <= 0 || h <= 0 -> "RES --"
                    scale > 1 -> "RES ${w}x$h -> ${w * scale}x${h * scale} (${scale}x)"
                    else -> "RES ${w}x$h"
                },
            )
        }
        if (showRenderer) {
            val name = GameOsd.renderer.value
            if (name.isNotEmpty()) add("GFX $name")
        }
    }

    // Branding header, prepended ONLY when there is a block to head — deriving it here rather
    // than adding it inside buildList keeps "every row off" meaning no OSD at all, which a
    // header added unconditionally would have quietly broken.
    val statLines = if (statRows.isEmpty()) statRows else listOf("ARMSX1") + statRows

    Box(Modifier.fillMaxSize()) {
        // Hidden while the pause overlay is up. The stats block sits top-RIGHT (matching ARMSX2)
        // and the pause menu plus its tab rail occupy exactly that space, so the rows end up
        // half-buried under the panel — unreadable, and the fragments that do show still carry
        // last-frame numbers, which reads as "the emulation is still running" when it is in fact
        // paused. The menu shows the session's own state anyway.
        if (statLines.isNotEmpty() && !overlayOpen) {
            // Top-RIGHT, matching ARMSX2. The rows are right-aligned to each other too, so the
            // block keeps a clean edge against the screen instead of a ragged one as figures
            // change width (280.0K -> 1.2M) every half-second refresh.
            Column(
                Modifier
                    .align(Alignment.TopEnd)
                    .padding(end = 10.dp, top = 8.dp),
                horizontalAlignment = Alignment.End,
                verticalArrangement = Arrangement.spacedBy(1.dp),
            ) {
                // softWrap = false: these rows are single lines of aligned figures, and a narrow
                // window (portrait, or a small multi-window pane) was wrapping the widest of them
                // — "FRAME ... worst" spilling its last value onto a second, left-hanging line,
                // which breaks the right-aligned column and reads as a rendering glitch. Clipping
                // the overflow keeps the block coherent; the rows that matter most are the short
                // ones, and the OSD is a glance-at readout, not a document.
                statLines.forEach { line ->
                    Text(line, style = textStyle, softWrap = false, maxLines = 1)
                }
            }
        }

        if (showNotes && GameOsd.notes.isNotEmpty()) {
            Column(
                Modifier
                    .align(Alignment.TopCenter)
                    .padding(top = 8.dp),
                horizontalAlignment = Alignment.CenterHorizontally,
                verticalArrangement = Arrangement.spacedBy(4.dp),
            ) {
                GameOsd.notes.forEach { note ->
                    key(note.id) {
                        Text(
                            note.text,
                            style = textStyle,
                            modifier = Modifier
                                .clip(RoundedCornerShape(6.dp))
                                .background(Color.Black.copy(alpha = 0.55f))
                                .padding(horizontal = 10.dp, vertical = 4.dp),
                        )
                    }
                }
            }
        }
    }
}
