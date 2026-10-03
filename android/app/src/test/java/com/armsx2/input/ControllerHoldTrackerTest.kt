package com.armsx2.input

import org.junit.Assert.*
import org.junit.Test

class ControllerHoldTrackerTest {
    @Test fun chordHoldStopsWhenEitherButtonIsReleased() {
        for (release in listOf(96, 97)) {
            val tracker = ControllerHoldTracker()
            val released = mutableListOf<ControllerHoldTracker.Action>()
            assertTrue(tracker.press(3, 97, ControllerHoldTracker.Action.REWIND, setOf(96, 97)))
            assertTrue(tracker.release(3, release, released::add))
            assertEquals(listOf(ControllerHoldTracker.Action.REWIND), released)
            assertFalse(tracker.release(3, if (release == 96) 97 else 96, released::add))
        }
    }

    @Test fun anotherControllersHoldIsNotReleasedByTheFirstController() {
        val tracker = ControllerHoldTracker()
        val released = mutableListOf<ControllerHoldTracker.Action>()
        assertTrue(tracker.press(3, 97, ControllerHoldTracker.Action.REWIND, setOf(96, 97)))
        assertFalse(tracker.press(4, 97, ControllerHoldTracker.Action.REWIND, setOf(96, 97)))
        assertTrue(tracker.release(3, 96, released::add))
        assertTrue(released.isEmpty())
        assertTrue(tracker.release(4, 97, released::add))
        assertEquals(listOf(ControllerHoldTracker.Action.REWIND), released)
    }

    @Test fun singleHoldAndResetStillReleaseExactlyOnce() {
        val tracker = ControllerHoldTracker()
        val released = mutableListOf<ControllerHoldTracker.Action>()
        assertTrue(tracker.press(3, 96, ControllerHoldTracker.Action.FAST_FORWARD))
        assertFalse(tracker.press(3, 96, ControllerHoldTracker.Action.FAST_FORWARD))
        tracker.reset(released::add)
        assertEquals(listOf(ControllerHoldTracker.Action.FAST_FORWARD), released)
        assertFalse(tracker.release(3, 96, released::add))
    }
}
