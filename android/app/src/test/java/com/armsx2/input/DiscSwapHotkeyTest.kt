package com.armsx2.input

import org.junit.Assert.assertEquals
import org.junit.Test

class DiscSwapHotkeyTest {
    @Test fun existingPersistedHotkeyOrdinalsAreUnchanged() {
        val previousNames = listOf(
            "MENU", "SAVE_STATE", "LOAD_STATE", "CYCLE_SLOT", "TEXTURE_DUMP",
            "SCREENSHOT", "TOGGLE_OSD", "FAST_FORWARD", "FAST_FORWARD_TOGGLE",
            "SLOW_DOWN", "RES_UP", "RES_DOWN", "ACHIEVEMENTS", "CLOSE_GAME",
            "QUIT_APP", "SAVE_AND_EXIT", "RESET_GAME", "PRESSURE_MOD",
            "GYRO_TOGGLE", "GYRO_HOLD", "TOGGLE_KEYBOARD", "GYRO_RECENTER", "REWIND",
        )
        assertEquals(previousNames, ControllerMappings.SysHotkey.entries.take(previousNames.size).map { it.name })
        assertEquals(previousNames.size, ControllerMappings.SysHotkey.SWAP_DISC.ordinal)
    }

    @Test fun swapDiscHasItsOwnPersistentBindingAndLabel() {
        assertEquals("pad.swapdisc.keycode", ControllerMappings.SysHotkey.SWAP_DISC.prefKey)
        assertEquals("Swap Disc", ControllerMappings.SysHotkey.SWAP_DISC.label)
    }
}
