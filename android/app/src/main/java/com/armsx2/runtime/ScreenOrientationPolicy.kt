package com.armsx2.runtime

import android.content.pm.ActivityInfo

/** The launcher and game/BIOS deliberately have independent orientation preferences. */
internal object ScreenOrientationPolicy {
    fun requested(emulating: Boolean, launcherMode: Int, emulationMode: Int): Int =
        when (if (emulating) emulationMode else launcherMode) {
            // SENSOR_* explicitly requests an axis even with Android auto-rotate disabled.
            1 -> ActivityInfo.SCREEN_ORIENTATION_SENSOR_LANDSCAPE
            2 -> ActivityInfo.SCREEN_ORIENTATION_SENSOR_PORTRAIT
            3 -> ActivityInfo.SCREEN_ORIENTATION_FULL_SENSOR
            else -> ActivityInfo.SCREEN_ORIENTATION_UNSPECIFIED
        }
}
