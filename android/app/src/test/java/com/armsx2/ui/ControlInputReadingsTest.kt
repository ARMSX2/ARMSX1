package com.armsx2.ui

import org.junit.Assert.*
import org.junit.Test

class ControlInputReadingsTest {
    @Test fun readsNativePlaystationButtonOrderAndMultiplePorts() {
        val values = IntArray(160)
        values[0] = 1; values[8] = 1; values[10] = 1
        values[20 + 4] = 1
        assertEquals(listOf("P1  × ○ L1", "P2  ↑"), ControlInputReadings.lines(values))
    }
    @Test fun showsSignedSticksAndIgnoresCenterNoise() {
        val values = IntArray(20)
        values[16] = 100
        assertEquals(listOf("P1  -"), ControlInputReadings.lines(values))
        values[16] = 32767; values[17] = -32768
        assertEquals(listOf("P1  LS(+1.00,-1.00)"), ControlInputReadings.lines(values))
    }
    @Test fun incompleteSnapshotIsSafe() {
        assertEquals(listOf("P1  -"), ControlInputReadings.lines(intArrayOf(1, 1)))
    }
}
