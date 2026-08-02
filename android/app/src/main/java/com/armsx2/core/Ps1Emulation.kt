package com.armsx2.core

import android.content.Context
import com.armsx2.config.Ps1Settings
import com.armsx2.config.Ps1SettingsStore
import kr.co.iefriends.pcsx2.NativeApp

/**
 * Pushes the emulation-behaviour settings that the core can change while a game runs:
 * `[input] multitap` and `[emulation] rewind` / `runahead`.
 *
 * Mirrors [Ps1Pacing] and [Ps1Display] on purpose — same shape, same call sites — so a new
 * settings screen cannot forget one of the three. Every setting here has a real native behind
 * it (`NativeApp.setMultitapEnabled`, `setRewind`, `setRunahead`), each of which parks the
 * work for the emulation thread; none needs a running VM, and with nothing booted the values
 * are simply re-applied from `settings.toml` at the next launch.
 *
 * The reason this file exists at all is the failure shape this project keeps hitting: a real
 * native with zero callers looks exactly like a dead setting. Writing `settings.toml` is not
 * enough on its own — the running game would keep the values it booted with, and the user
 * would report the control as doing nothing.
 */
object Ps1Emulation {

    /** Read the persisted settings and push them. Cheap; safe with no VM running.
     *  Resolved for the ACTIVE scope so a per-game override wins, like the other two. */
    fun push(context: Context) {
        val s = runCatching { Ps1SettingsStore.active(context) }.getOrNull() ?: return
        push(s)
    }

    /** Push an in-memory snapshot, for callers that already hold one. */
    fun push(s: Ps1Settings) {
        runCatching {
            NativeApp.setMultitapEnabled(s.multitap)
            // Passing enabled = false frees the snapshot ring immediately, which is the whole
            // point of routing the switch through the core rather than only through the file.
            NativeApp.setRewind(s.rewind, s.rewindSeconds, s.rewindFrequency)
            NativeApp.setRunahead(s.runahead)
        }
    }

    /**
     * Bytes ONE rewind snapshot costs, straight from the core: measured from the running
     * machine once a state has been captured, an estimate before that. Used to label the
     * rewind rows with what they actually cost in memory rather than only in seconds.
     */
    fun snapshotBytes(): Long =
        runCatching { NativeApp.rewindSnapshotBytes() }
            .getOrDefault(Ps1Settings.REWIND_ESTIMATED_SNAPSHOT_BYTES)
            .takeIf { it > 0 } ?: Ps1Settings.REWIND_ESTIMATED_SNAPSHOT_BYTES

    /** Bytes the ring is holding right now; 0 while rewind is off. */
    fun bytesHeld(): Long = runCatching { NativeApp.rewindBytesHeld() }.getOrDefault(0L)

    /** Hold-to-rewind, from a bound hotkey or an on-screen control. */
    fun setRewindActive(active: Boolean) {
        runCatching { NativeApp.setRewindActive(active) }
    }

    /** One-shot step back, for a menu row. */
    fun stepBack() {
        runCatching { NativeApp.rewindStepBack() }
    }
}
