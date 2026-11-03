package com.armsx2.runtime

import com.armsx2.runtime.DualScreen.Mode
import com.armsx2.runtime.DualScreen.Placement
import org.junit.Assert.assertEquals
import org.junit.Test

class DualScreenPlacementTest {
    private fun place(
        mode: Mode,
        second: Boolean = true,
        inGame: Boolean = true,
        edit: Boolean = false,
        companion: Boolean = false,
    ) = DualScreen.placement(mode, second, inGame, edit, companion)

    @Test fun offNeverUsesTheSecondScreen() {
        assertEquals(Placement.NONE, place(Mode.OFF))
        assertEquals(Placement.NONE, place(Mode.OFF, edit = true))
    }

    @Test fun singleDisplayIsUnchangedInEveryMode() {
        Mode.entries.forEach { assertEquals(Placement.NONE, place(it, second = false)) }
    }

    @Test fun gameOnMainPutsControlsOnTheOtherScreen() {
        assertEquals(Placement.CONTROLS_ON_SECOND, place(Mode.GAME_MAIN))
        assertEquals(Placement.CONTROLS_ON_SECOND, place(Mode.GAME_MAIN, inGame = false, edit = true))
        assertEquals(Placement.NONE, place(Mode.GAME_MAIN, inGame = false))
    }

    @Test fun gameOnSecondMovesOnlyARunningGame() {
        assertEquals(Placement.GAME_ON_SECOND, place(Mode.GAME_SECOND))
        assertEquals(Placement.NONE, place(Mode.GAME_SECOND, inGame = false, edit = true))
    }

    @Test fun ctrCompanionKeepsTheSecondScreen() {
        Mode.entries.forEach { assertEquals(Placement.NONE, place(it, companion = true)) }
    }

    @Test fun libraryArtOnlyOutOfAGame() {
        Mode.entries.filter { it != Mode.OFF }.forEach { mode ->
            assertEquals(Placement.LIBRARY_ON_SECOND,
                DualScreen.placement(mode, true, inGame = false, touchEditMode = false, companionShowing = false, libraryArt = true))
            assertEquals(Placement.NONE,
                DualScreen.placement(mode, true, inGame = false, touchEditMode = false, companionShowing = false, libraryArt = false))
        }
        assertEquals(Placement.NONE,
            DualScreen.placement(Mode.OFF, true, inGame = false, touchEditMode = false, companionShowing = false, libraryArt = true))
        assertEquals(Placement.CONTROLS_ON_SECOND,
            DualScreen.placement(Mode.GAME_MAIN, true, inGame = true, touchEditMode = false, companionShowing = false, libraryArt = true))
    }

    @Test fun unknownStoredModeFallsBackToOff() {
        assertEquals(Mode.OFF, Mode.fromKey(null))
        assertEquals(Mode.OFF, Mode.fromKey("bogus"))
        assertEquals(Mode.GAME_SECOND, Mode.fromKey("game_second"))
    }
}
