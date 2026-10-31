package com.armsx2.ui.home

/** Immediate controller-follow math, independent of animation and Compose. */
internal object HomeScrollPolicy {
    fun revealDelta(start: Int, size: Int, viewportStart: Int, viewportEnd: Int): Int {
        if (size <= 0 || viewportEnd <= viewportStart) return 0
        val end = start.toLong() + size
        // A row taller than the viewport cannot fit: leave it alone while it spans
        // the viewport rather than bouncing between its two edges on every move.
        if (size > viewportEnd.toLong() - viewportStart) {
            return when {
                start > viewportStart -> start - viewportStart
                end < viewportEnd -> (end - viewportEnd).toInt()
                else -> 0
            }
        }
        return when {
            start < viewportStart -> start - viewportStart
            end > viewportEnd -> (end - viewportEnd).toInt()
            else -> 0
        }
    }

    fun targetIndex(selection: Int, leading: Int, gamesPerItem: Int): Int =
        leading.coerceAtLeast(0) + selection.coerceAtLeast(0) / gamesPerItem.coerceAtLeast(1)

    fun adaptiveColumns(width: Float, minCell: Float, spacing: Float, padding: Float): Int =
        ((width - padding + spacing.coerceAtLeast(0f)) /
            (minCell.coerceAtLeast(1f) + spacing.coerceAtLeast(0f))).toInt().coerceAtLeast(1)
}
