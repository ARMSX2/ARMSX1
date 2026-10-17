package com.armsx2.ui.home

/** Compact presentation of cached results; never performs a hash or a network request. */
internal data class LibraryAchievementStatus(
    val earned: Int, val total: Int, val hardcore: Boolean, val softcore: Int,
) {
    val mastered get() = hardcore && earned == total
    val completed get() = !hardcore && earned == total
    val label get() = "$earned/$total" + if (hardcore) " HC" else ""

    companion object {
        fun from(softcore: Int, hardcore: Int, total: Int): LibraryAchievementStatus? {
            if (total <= 0) return null
            val hc = hardcore.coerceIn(0, total)
            val sc = softcore.coerceIn(hc, total)
            return LibraryAchievementStatus(if (hc > 0) hc else sc, total, hc > 0, sc)
        }
    }
}
