package com.armsx2.ui.emulation

import org.junit.Assert.*
import org.junit.Test

class MenuOrderPolicyTest {
    @Test fun newDiscordOptionFollowsScreenshotInCustomLayout() {
        val custom = listOf("action.close", "action.screenshot", "action.resume")
        assertEquals(listOf("action.close", "action.screenshot", "action.discord", "action.resume"),
            MenuOrderPolicy.insertAfterIfNew(custom, "action.discord", "action.screenshot"))
    }
    @Test fun existingDiscordPositionIsPreserved() {
        val custom = listOf("action.discord", "action.screenshot", "action.resume")
        assertEquals(custom, MenuOrderPolicy.insertAfterIfNew(custom, "action.discord", "action.screenshot"))
        assertEquals(emptyList<String>(), MenuOrderPolicy.insertAfterIfNew(emptyList(), "action.discord", "action.screenshot"))
    }

    @Test fun savedOrderSurvivesHiddenOptionsAndNewOptions() {
        val stored = listOf("resume", "disc2", "swap", "close", "resume")
        assertEquals(listOf("resume", "swap", "new"), MenuOrderPolicy.visible(stored, listOf("swap", "resume", "new")))
    }
    @Test fun movePreservesHiddenDiscSoItsOrderReturnsWhenAvailable() {
        val stored = listOf("resume", "disc2", "swap", "close")
        val moved = MenuOrderPolicy.move(stored, listOf("resume", "swap", "close"), "close", "resume")
        assertEquals(listOf("close", "resume", "disc2", "swap"), moved)
        assertEquals(moved, MenuOrderPolicy.visible(moved, stored))
    }
    @Test fun movesInBothDirectionsAndRejectsMissingTargets() {
        val defaults = listOf("session", "graphics", "controls")
        val down = MenuOrderPolicy.move(emptyList(), defaults, "session", "controls")
        assertEquals(listOf("graphics", "controls", "session"), down)
        assertEquals(defaults, MenuOrderPolicy.move(down, defaults, "session", "graphics"))
        assertEquals(defaults, MenuOrderPolicy.move(defaults, defaults, "session", "missing"))
    }
    @Test fun shortPressActivatesOnlyOnceOnItsMatchingRelease() {
        val press = MenuConfirmPress()
        assertTrue(press.down(96))
        assertFalse(press.down(96)) // repeats cannot re-arm a press
        assertFalse(press.up(23, false)) // unrelated release cannot confirm
        assertTrue(press.up(96, false))
        assertFalse(press.up(96, false))
    }
    @Test fun holdNeverConfirmsAndNextPressCanSave() {
        val press = MenuConfirmPress()
        press.down(96)
        press.held()
        assertFalse(press.down(96))
        assertFalse(press.up(96, false))
        assertTrue(press.down(96))
        assertTrue(press.up(96, false))
    }
    @Test fun NavigationFocusLossAndCanceledReleaseDoNotActivate() {
        val press = MenuConfirmPress()
        press.down(96); press.cancel()
        assertFalse(press.up(96, false))
        press.down(96); press.reset()
        assertFalse(press.up(96, false))
        press.down(96)
        assertFalse(press.up(96, true))
    }
}
