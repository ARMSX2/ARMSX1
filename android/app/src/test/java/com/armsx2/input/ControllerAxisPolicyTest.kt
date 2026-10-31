package com.armsx2.input

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class ControllerAxisPolicyTest {
    private val signed = ControllerAxisPolicy.Range(-1f, 1f)

    @Test fun normalizesDifferentControllerRanges() {
        for (range in listOf(signed, ControllerAxisPolicy.Range(-32768f, 32767f),
            ControllerAxisPolicy.Range(0f, 255f))) {
            assertEquals(-1f, ControllerAxisPolicy.centered(range.min, range), 0.0001f)
            assertEquals(1f, ControllerAxisPolicy.centered(range.max, range), 0.0001f)
            val center = if (range.min < 0f) 0f else 127.5f
            assertEquals(0f, ControllerAxisPolicy.centered(center, range), 0.0001f)
        }
        assertEquals(0f, ControllerAxisPolicy.centered(Float.NaN, signed), 0f)
        assertEquals(0f, ControllerAxisPolicy.centered(0.1f, signed.copy(flat = 0.15f)), 0f)
    }

    @Test fun prefersOnlyCompleteRightStickPairs() {
        assertTrue(ControllerAxisPolicy.useRotationalRightStick(signed, null, signed, signed, false))
        assertTrue(ControllerAxisPolicy.useRotationalRightStick(signed,
            ControllerAxisPolicy.Range(0f, 1f), signed, signed, false))
        assertFalse(ControllerAxisPolicy.useRotationalRightStick(signed, signed, signed, null, true))
        assertFalse(ControllerAxisPolicy.useRotationalRightStick(signed, signed, signed, signed, false))
    }

    @Test fun signedTriggerRestDoesNotPressButton() {
        assertEquals(0f, ControllerAxisPolicy.trigger(-1f, signed), 0f)
        assertEquals(0.5f, ControllerAxisPolicy.trigger(0f, signed), 0f)
        assertEquals(1f, ControllerAxisPolicy.trigger(1f, signed), 0f)
    }
}
