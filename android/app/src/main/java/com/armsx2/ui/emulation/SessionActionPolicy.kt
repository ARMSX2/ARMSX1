package com.armsx2.ui.emulation

/** Controller dispatch and the visible grid share stable action IDs, never shifted indices. */
internal object SessionActionPolicy {
    // MUST match SessionPane's MenuAction order exactly: the pad activates by index. Save State,
    // Load State and Discord are gone from the tab (Discord is the header button now), so they are
    // gone here too -- leaving them would shift every row below and make A on Fast-Forward open
    // the save-state picker.
    fun ids(rewind: Boolean): List<String> = listOf(
        "resume", "fastForward", "reset", "swapDisc", "swapGame", "close", "screenshot",
    ) + if (rewind) listOf("rewind") else emptyList()
}
