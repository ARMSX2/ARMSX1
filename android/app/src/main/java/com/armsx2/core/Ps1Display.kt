package com.armsx2.core

import android.content.Context
import com.armsx2.config.Ps1Settings
import com.armsx2.config.Ps1SettingsStore
import kr.co.iefriends.pcsx2.NativeApp

/**
 * Pushes the presentation-only display settings into a RUNNING session.
 *
 * These are pure presentation properties — nothing in the emulated machine depends on them —
 * so the core applies them on the very next presented frame, with no VM restart.
 *
 * This exists because the native side was already complete and unreachable:
 * `NativeApp.setDisplayAspect` / `setStretchMode` were real `native` methods with **zero
 * callers**, so changing Display aspect or Stretch wrote `settings.toml` and never reached
 * the running game. It looked exactly like a dead setting, and it is the same failure shape
 * that has hit pads, save states, fast-forward and the FPS cap in this port: an implemented
 * end with nothing calling it.
 *
 * Mirrors [Ps1Pacing] deliberately — same shape, same call sites — so neither is forgotten
 * when a new settings screen is added.
 */
object Ps1Display {

    /** Read the persisted settings and push them. Cheap; safe with no VM running.
     *  Resolved for the ACTIVE scope, so a game that pins its own display mode keeps it. */
    fun push(context: Context) {
        val s = runCatching { Ps1SettingsStore.active(context) }.getOrNull() ?: return
        push(s)
    }

    /** Push an in-memory snapshot, for callers that already hold one. */
    fun push(s: Ps1Settings) {
        val mode = Ps1Settings.ASPECTS.indexOf(s.displayAspect).takeIf { it >= 0 } ?: 0
        runCatching {
            // Order matters only in that the ratio should be in place before the mode selects
            // it, so a switch to Custom never presents one frame at a stale ratio.
            NativeApp.setDisplayAspectCustom(s.displayAspectCustom)
            NativeApp.setDisplayAspect(mode)
            NativeApp.setStretchMode(s.stretchMode)
            NativeApp.setIntegerScaling(s.integerScaling)
        }
    }
}
