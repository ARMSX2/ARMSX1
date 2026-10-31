package com.armsx2.input

import kotlin.math.abs

object ControllerAxisPolicy {
    data class Range(val min: Float, val max: Float, val flat: Float = 0f)

    fun centered(value: Float, range: Range?): Float {
        if (!value.isFinite()) return 0f
        if (range == null) return value.coerceIn(-1f, 1f)
        if (!range.min.isFinite() || !range.max.isFinite() || range.max <= range.min) return 0f
        val center = if (range.min < 0f && range.max > 0f) 0f else (range.min + range.max) * 0.5f
        val delta = value - center
        val span = if (delta < 0f) center - range.min else range.max - center
        if (span <= 0f || abs(delta) <= range.flat.coerceIn(0f, span * 0.95f)) return 0f
        return (delta / span).coerceIn(-1f, 1f)
    }

    fun trigger(value: Float, range: Range?): Float {
        if (!value.isFinite()) return 0f
        if (range == null) return value.coerceIn(0f, 1f)
        if (!range.min.isFinite() || !range.max.isFinite() || range.max <= range.min) return 0f
        return ((value - range.min) / (range.max - range.min)).coerceIn(0f, 1f)
    }

    fun useRotationalRightStick(z: Range?, rz: Range?, rx: Range?, ry: Range?, preferRotational: Boolean): Boolean {
        if (rx == null || ry == null) return false
        return preferRotational || z == null || rz == null ||
            (rz.min >= 0f && rz.max <= 1f &&
                (rx.min < 0f || rx.max - rx.min > 1f) && (ry.min < 0f || ry.max - ry.min > 1f))
    }
}
