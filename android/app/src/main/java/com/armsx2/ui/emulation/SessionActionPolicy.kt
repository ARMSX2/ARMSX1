package com.armsx2.ui.emulation

/** Controller dispatch and the visible grid share stable action IDs, never shifted indices. */
internal object SessionActionPolicy {
    fun ids(rewind: Boolean): List<String> = listOf(
        "resume", "saveState", "loadState", "fastForward", "reset", "swapDisc", "swapGame",
        "close", "screenshot", "discord",
    ) + if (rewind) listOf("rewind") else emptyList()
}
