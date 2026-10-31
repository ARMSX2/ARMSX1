package com.armsx2.update

import androidx.compose.runtime.Composable

/**
 * Play-flavor no-op stub of the in-app updater. Play forbids self-updating apps, so the real
 * implementation (network check, APK download, REQUEST_INSTALL_PACKAGES install) lives ONLY in
 * src/github. Shared code calls these gated on BuildConfig.IN_APP_UPDATER — AppTab (the General
 * settings tab) calls UpdaterEntry(), MainActivityRuntime's app root calls AutoUpdateGate() — and
 * this stub lets the play flavor compile those references while shipping nothing. The play build
 * must never gain the updater code or permission; two independent guards fail closed if it does:
 * the :app:verifyPlayFlavorClean* Gradle tasks, which finalize every play assemble/bundle, and
 * build-play-aab.sh, which re-checks the finished .aab.
 */
@Composable
fun UpdaterEntry() {
    // intentionally empty
}

/** Play-flavor no-op stub of the boot-time auto-update check (real one in src/github). */
@Composable
fun AutoUpdateGate() {
    // intentionally empty
}
