package com.armsx2.core

import android.content.Context
import com.armsx2.config.Ps1Settings
import com.armsx2.config.Ps1SettingsStore
import kr.co.iefriends.pcsx2.NativeApp

/**
 * The one place the frame-pacing policy lives on the Kotlin side.
 *
 * Four values — frame limit, emulation speed %, absolute fps cap, fast-forward multiplier — that
 * the PS1 core folds into a single target rate (`ArmsxSession::targetFrameRate`), plus frame skip,
 * which shares the table and the push but decides something else entirely (how many of the frames
 * the machine produces are actually drawn). They are persisted in `settings.toml`'s `[runtime]`
 * table, which the core reads at boot, and pushed live through `NativeApp.setSpeedLimits()` so the
 * in-game pause menu applies without a relaunch. `settings.toml` is the source of truth; this
 * object is a cache in front of it, not a second store.
 *
 * ★ What this REPLACED matters more than what it does. Every one of these controls used to end in
 * `NativeApp.setSetting("EmuCore/GS", "FrameLimitEnable", …)` / `speedhackLimitermode()` /
 * `setTurboScalar()` — PCSX2 entry points that are empty stubs in this port. The switches moved,
 * the values persisted, and the emulation thread never heard a thing. Those call sites are gone;
 * do not reintroduce them alongside this.
 */
object Ps1Pacing {

    @Volatile
    private var cached: Ps1Settings? = null

    /** Current values, read from disk once and then cached. Safe to call from any thread.
     *
     *  Resolved for the ACTIVE scope — the running game's merge of global + its own overrides, or
     *  plain global with nothing running. The launcher invalidates this cache when it switches the
     *  active game, so a per-game frame cap cannot leak into the next title. */
    fun settings(context: Context): Ps1Settings =
        cached ?: runCatching { Ps1SettingsStore.active(context) }.getOrDefault(Ps1Settings())
            .also { cached = it }

    /** Forget the cache — call after something else rewrote settings.toml (the settings tabs). */
    fun invalidate() {
        cached = null
    }

    fun frameLimit(context: Context): Boolean = settings(context).frameLimit

    fun speedPercent(context: Context): Int = settings(context).speedPercent.coerceIn(10, 1000)

    fun fpsLimit(context: Context): Int = settings(context).fpsLimit.coerceIn(0, 1000)

    /** Fast-forward multiplier; 0 means uncapped. */
    fun fastForwardSpeed(context: Context): Float = settings(context).fastForwardSpeed.let {
        if (it <= 0f) 0f else it.coerceIn(1f, 16f)
    }

    /**
     * Frame skip: 0 off, -1 adaptive, 1..5 fixed.
     *
     * Lives here because it is edited from the same panels and pushed by the same call, but it is
     * NOT a pacing value — the core drops PRESENTS with it and never an emulated frame, so the
     * game, its audio and its timing run exactly as they would with it off.
     */
    fun frameSkip(context: Context): Int = settings(context).frameSkip.coerceIn(-1, 5)

    fun setFrameLimit(context: Context, enabled: Boolean) =
        update(context) { it.copy(frameLimit = enabled) }

    fun setSpeedPercent(context: Context, percent: Int) =
        update(context) { it.copy(speedPercent = percent.coerceIn(10, 1000)) }

    fun setFpsLimit(context: Context, fps: Int) =
        update(context) { it.copy(fpsLimit = fps.coerceIn(0, 1000)) }

    fun setFastForwardSpeed(context: Context, speed: Float) =
        update(context) { it.copy(fastForwardSpeed = if (speed <= 0f) 0f else speed.coerceIn(1f, 16f)) }

    fun setFrameSkip(context: Context, skip: Int) =
        update(context) { it.copy(frameSkip = skip.coerceIn(-1, 5)) }

    /**
     * Write one field back and push the whole policy to the core.
     *
     * The transform runs against what is ON DISK rather than the cache: the settings tabs rewrite
     * the entire file from their own snapshot, so regenerating from a stale cache here would roll
     * their edits back (the same trap `Ps1SettingsEditor.update` documents).
     */
    private fun update(context: Context, transform: (Ps1Settings) -> Ps1Settings) {
        val app = context.applicationContext
        // Writes the GLOBAL layer, which is what these call sites (hotkeys, the pause menu's speed
        // rows) have always meant — they are the user's default pacing, not a property of one disc.
        // The cache is then re-resolved for the ACTIVE scope, so if the running game happens to pin
        // one of these keys the cache keeps showing the value the core is really using rather than
        // the global one that lost.
        runCatching { Ps1SettingsStore.update(app, null, transform) }
        cached = runCatching { Ps1SettingsStore.active(app) }.getOrNull()
        push(app)
    }

    /**
     * Hand the current policy to the running core. Also called right before fast-forward is
     * engaged, so a multiplier changed while no game was running still applies to this session.
     * Harmless with no VM: the core parks the request and applies it when a session next starts.
     */
    fun push(context: Context) {
        val s = settings(context.applicationContext)
        runCatching {
            NativeApp.setSpeedLimits(
                s.frameLimit,
                s.speedPercent.coerceIn(10, 1000),
                s.fpsLimit.coerceIn(0, 1000),
                if (s.fastForwardSpeed <= 0f) 0f else s.fastForwardSpeed.coerceIn(1f, 16f),
                s.frameSkip.coerceIn(-1, 5),
            )
        }
    }

    /** "2×" / "Unlimited", for the OSD note and the menu rows. */
    fun fastForwardLabel(context: Context): String =
        Ps1Settings.fastForwardSpeedLabel(fastForwardSpeed(context))
}
