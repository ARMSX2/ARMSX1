package com.armsx2.config

/** Bits mirror the two physical PlayStation ports, not a controller count. */
enum class MultiplayerMode(val nativeValue: Int, val players: Int) {
    STANDARD(0, 2), FOUR_PLAYERS(1, 4), PORT2_ONLY(2, 4), EIGHT_PLAYERS(3, 8);

    val port1: Boolean get() = nativeValue and 1 != 0
    val port2: Boolean get() = nativeValue and 2 != 0
    fun overrides(): Map<String, String> = mapOf("input.multitap" to port1.toString(), "input.multitap_port2" to port2.toString())

    companion object {
        val presets = listOf(STANDARD, FOUR_PLAYERS, EIGHT_PLAYERS)
        fun fromPorts(port1: Boolean, port2: Boolean): MultiplayerMode =
            entries.first { it.nativeValue == (if (port1) 1 else 0) + (if (port2) 2 else 0) }
    }
}
