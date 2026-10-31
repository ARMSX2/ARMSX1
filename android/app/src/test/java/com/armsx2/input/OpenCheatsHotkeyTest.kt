package com.armsx2.input

import org.junit.Assert.*
import org.junit.Test

class OpenCheatsHotkeyTest {
    @Test fun newActionIsAppendedWithoutChangingExistingStickBindingOrdinals() {
        val previous = listOf("MENU", "SAVE_STATE", "LOAD_STATE", "CYCLE_SLOT", "TEXTURE_DUMP",
            "SCREENSHOT", "TOGGLE_OSD", "FAST_FORWARD", "FAST_FORWARD_TOGGLE", "SLOW_DOWN",
            "RES_UP", "RES_DOWN", "ACHIEVEMENTS", "CLOSE_GAME", "QUIT_APP", "SAVE_AND_EXIT",
            "RESET_GAME", "PRESSURE_MOD", "GYRO_TOGGLE", "GYRO_HOLD", "TOGGLE_KEYBOARD",
            "GYRO_RECENTER", "REWIND", "SWAP_DISC")
        assertEquals(previous, ControllerMappings.SysHotkey.entries.take(previous.size).map { it.name })
        assertEquals(previous.size, ControllerMappings.SysHotkey.OPEN_CHEATS.ordinal)
    }

    @Test fun openCheatsHasUniqueBindingAndAppearsInTheSharedHotkeysUi() {
        val action = ControllerMappings.SysHotkey.OPEN_CHEATS
        assertEquals("Open Cheats", action.label)
        assertEquals("pad.opencheats.keycode", action.prefKey)
        assertEquals(1, ControllerMappings.SysHotkey.entries.count { it.prefKey == action.prefKey })
        assertEquals(1, ControllerMappings.ps1Hotkeys.count { it == action })
        assertEquals(action, ControllerMappings.hotkeyForStickCode(ControllerMappings.stickCodeForHotkey(action)))
    }

    @Test fun sharedCaptureSupportsSingleButtonsAndOrderIndependentTwoButtonCombos() {
        val capture = HotkeyCapturePolicy()
        capture.down(106, 3, 317)
        assertEquals(CapturedHotkey(106), capture.up(106, 3))
        capture.down(109, 3, 314)
        capture.down(106, 3, 317)
        assertNull(capture.up(109, 3))
        assertEquals(CapturedHotkey(106, 109), capture.up(106, 3))
        assertTrue(HotkeyChordPolicy.matches(106, 109, 106, setOf(109)))
        assertTrue(HotkeyChordPolicy.matches(106, 109, 109, setOf(106)))
        assertFalse(HotkeyChordPolicy.matches(106, 109, 106, emptySet()))
    }
}
