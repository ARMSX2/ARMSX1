package com.armsx2.ui.touch

import kotlin.math.abs

/** Hit geometry shares the image's ContentScale.Fit footprint, without changing layout/sizing. */
internal object TouchHitGeometry {
    data class Extent(val width: Float, val height: Float) {
        fun contains(dx: Float, dy: Float): Boolean =
            abs(dx) <= width / 2f && abs(dy) <= height / 2f
    }

    fun fittedExtent(size: Float, imageWidth: Float, imageHeight: Float): Extent {
        if (!size.isFinite() || size <= 0f) return Extent(0f, 0f)
        if (!imageWidth.isFinite() || !imageHeight.isFinite() || imageWidth <= 0f || imageHeight <= 0f)
            return Extent(size, size)
        val scale = size / maxOf(imageWidth, imageHeight)
        return Extent(imageWidth * scale, imageHeight * scale)
    }

    fun contains(dx: Float, dy: Float, radius: Float, compact: Extent?): Boolean =
        compact?.contains(dx, dy) ?: (dx * dx + dy * dy <= radius * radius)
}
