package com.armsx2.input

import org.junit.Assert.*
import org.junit.Test

class HotkeyBindingPolicyTest {
    private val a = 96
    private val b = 97
    private val pad = 3

    @Test fun singleBindsOnlyAfterItsRelease() {
        val capture = HotkeyCapturePolicy()
        capture.down(a, pad, 304)
        assertEquals(listOf(a), capture.codes)
        assertNull(capture.up(b, pad))
        assertEquals(CapturedHotkey(a), capture.up(a, pad))
    }

    @Test fun simultaneousButtonsBindWithoutTimingGapAndWaitForBothReleases() {
        val capture = HotkeyCapturePolicy()
        capture.down(a, pad, 304)
        capture.down(b, pad, 305)
        assertEquals(listOf(a, b), capture.codes)
        assertNull(capture.up(a, pad))
        assertEquals(CapturedHotkey(b, a), capture.up(b, pad))
        assertTrue(capture.codes.isEmpty())
    }

    @Test fun eitherReleaseOrderSavesSamePair() {
        for (releasedFirst in listOf(a, b)) {
            val capture = HotkeyCapturePolicy()
            capture.down(a, pad, 304)
            capture.down(b, pad, 305)
            assertNull(capture.up(releasedFirst, pad))
            assertEquals(CapturedHotkey(b, a), capture.up(if (releasedFirst == a) b else a, pad))
        }
    }

    @Test fun repeatsAndDuplicateDownCannotCreateACombo() {
        val capture = HotkeyCapturePolicy()
        capture.down(a, pad, 304)
        capture.down(a, pad, 304)
        capture.down(b, pad, 305, repeat = true)
        assertEquals(CapturedHotkey(a), capture.up(a, pad))
    }

    @Test fun driverAliasesForOnePhysicalScanCodeStayASingleBinding() {
        val capture = HotkeyCapturePolicy()
        capture.down(a, pad, 304)
        capture.down(b, pad, 304)
        assertEquals(listOf(a), capture.codes)
        assertNull(capture.up(a, pad))
        assertEquals(CapturedHotkey(a), capture.up(b, pad))
    }

    @Test fun syntheticDirectionsWithNoScanCodeCanFormAPair() {
        val capture = HotkeyCapturePolicy()
        capture.down(19, pad, 0)
        capture.down(102, pad, 0)
        assertNull(capture.up(102, pad))
        assertEquals(CapturedHotkey(102, 19), capture.up(19, pad))
    }

    @Test fun differentControllersDoNotBecomeAChord() {
        val capture = HotkeyCapturePolicy()
        capture.down(a, pad, 304)
        capture.down(b, pad + 1, 305)
        assertNull(capture.up(b, pad + 1))
        assertEquals(CapturedHotkey(a), capture.up(a, pad))
    }

    @Test fun cancellingCaptureDoesNotSaveOldHeldButtons() {
        val capture = HotkeyCapturePolicy()
        capture.down(a, pad, 304)
        capture.reset()
        assertNull(capture.up(a, pad))
        capture.down(b, pad, 305)
        assertEquals(CapturedHotkey(b), capture.up(b, pad))
    }

    @Test fun chordMatchesEitherPressOrderButNeedsBothDistinctButtons() {
        assertTrue(HotkeyChordPolicy.matches(a, b, a, setOf(a, b)))
        assertTrue(HotkeyChordPolicy.matches(a, b, b, setOf(a, b)))
        assertFalse(HotkeyChordPolicy.matches(a, b, a, setOf(a)))
        assertFalse(HotkeyChordPolicy.matches(a, b, b, setOf(b)))
        assertFalse(HotkeyChordPolicy.matches(a, a, a, setOf(a)))
        assertFalse(HotkeyChordPolicy.matches(a, 0, a, setOf(a, 0)))
    }
}
