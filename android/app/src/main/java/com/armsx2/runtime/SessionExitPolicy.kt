package com.armsx2.runtime

/** Called only after the native run loop has returned and memory-card/state writes have ended. */
internal object SessionExitPolicy {
    enum class Destination { ExitApp, Restart, Library }

    fun destination(quitRequested: Boolean, restartRequested: Boolean): Destination = when {
        quitRequested -> Destination.ExitApp
        restartRequested -> Destination.Restart
        else -> Destination.Library
    }

    fun closeExitsApp(externalLaunch: Boolean, returnToLauncher: Boolean): Boolean =
        externalLaunch && returnToLauncher

    fun fullyStopped(runLoopActive: Boolean, stopInProgress: Boolean, nativeActive: Boolean): Boolean =
        !runLoopActive && !stopInProgress && !nativeActive
}
