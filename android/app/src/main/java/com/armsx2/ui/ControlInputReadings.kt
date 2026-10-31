package com.armsx2.ui

import java.util.Locale
import kotlin.math.abs

/** The labels follow the native libretro joypad order, not Android keycodes. */
internal object ControlInputReadings {
    private val labels = listOf("×", "□", "Select", "Start", "↑", "↓", "←", "→",
        "○", "△", "L1", "R1", "L2", "R2", "L3", "R3")
    fun lines(snapshot: IntArray): List<String> = (0 until minOf(8, snapshot.size / 20)).mapNotNull { port ->
        val base = port * 20
        val pressed = labels.filterIndexed { i, _ -> snapshot[base + i] != 0 }
        val sticks = (0..1).mapNotNull { stick ->
            val x = snapshot[base + 16 + stick * 2]
            val y = snapshot[base + 17 + stick * 2]
            if (abs(x.toLong()) < 1024 && abs(y.toLong()) < 1024) null
            else String.format(Locale.US, "%s(%+.2f,%+.2f)", if (stick == 0) "LS" else "RS",
                x.coerceIn(-32768, 32767) / 32768f, y.coerceIn(-32768, 32767) / 32768f)
        }
        val inputs = pressed + sticks
        if (inputs.isEmpty()) null else "P${port + 1}  ${inputs.joinToString(" ")}"
    }.ifEmpty { listOf("P1  -") }
}
