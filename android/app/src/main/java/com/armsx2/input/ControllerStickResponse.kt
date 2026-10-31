package com.armsx2.input

import kotlin.math.abs
import kotlin.math.hypot
import kotlin.math.pow

object ControllerStickResponse {
    fun magnitude(m: Float, deadzone: Float, outer: Float, acceleration: Float,
                  gamma: Float, sensitivity: Float, anti: Float): Float {
        if (!m.isFinite() || m <= deadzone) return 0f
        val hi = (1f - outer).coerceAtLeast(deadzone + 0.01f)
        val t = ((m - deadzone) / (hi - deadzone)).coerceIn(0f, 1f)
        val scaled = t.pow(1f + acceleration + gamma) * sensitivity
        return if (scaled > 0f) anti + scaled * (1f - anti) else 0f
    }

    fun vector(x: Float, y: Float, deadzone: Float, outer: Float, acceleration: Float,
               gamma: Float, sensitivity: Float, anti: Float): Pair<Float, Float> {
        if (!x.isFinite() || !y.isFinite()) return 0f to 0f
        var gx = x.coerceIn(-1f, 1f)
        var gy = y.coerceIn(-1f, 1f)
        if (abs(gx) >= abs(gy)) {
            if (abs(gy) < abs(gx) * 0.15f) gy = 0f
        } else if (abs(gx) < abs(gy) * 0.15f) gx = 0f
        val radius = hypot(gx, gy)
        if (radius == 0f) return 0f to 0f
        val scale = magnitude(radius, deadzone, outer, acceleration, gamma, sensitivity, anti) /
            radius.coerceAtMost(1f)
        return (gx * scale).coerceIn(-1f, 1f) to (gy * scale).coerceIn(-1f, 1f)
    }

    @JvmStatic
    fun axisByte(positive: Int, negative: Int): Int {
        val delta = positive.coerceIn(0, 32767) - negative.coerceIn(0, 32767)
        return 128 + delta * (if (delta < 0) 128 else 127) / 32767
    }
}
