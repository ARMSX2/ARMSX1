package com.armsx2.core

import java.util.concurrent.Executors
import java.util.concurrent.atomic.AtomicBoolean

/**
 * Applies an internal-resolution or rasterizer change to the game that is running.
 *
 * PSX Lab fixes both when a game's video starts, so they used to wait for a restart.
 * [kr.co.iefriends.pcsx2.NativeApp.reloadVideo] rebuilds the video in place from a state of the
 * running machine, the way a state load does, and the game carries on from the same moment.
 *
 * One rebuild at a time, off the UI thread. Changes made while one is running collapse into a
 * single follow-up, which reads whatever the settings are by then.
 */
object Ps1VideoReload {
    private val worker = Executors.newSingleThreadExecutor { task -> Thread(task, "armsx-reload-video") }
    private val queued = AtomicBoolean(false)

    fun request() {
        if (!queued.compareAndSet(false, true)) return
        worker.execute {
            queued.set(false)
            if (runCatching { kr.co.iefriends.pcsx2.NativeApp.hasActiveVM() }.getOrDefault(false)) {
                runCatching { kr.co.iefriends.pcsx2.NativeApp.reloadVideo() }
            }
        }
    }
}
