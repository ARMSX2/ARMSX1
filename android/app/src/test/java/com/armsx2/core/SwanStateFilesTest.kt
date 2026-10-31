package com.armsx2.core
import org.junit.Assert.*
import org.junit.Test
class SwanStateFilesTest {
    @Test fun formatsAreNotInterchangeable() {
        assertNull(SwanStateFiles.slot("Crash-01234567.slot1.pss"))
        assertEquals(1, SwanStateFiles.slot("SLUS-00594-abc123-1.swanstate"))
        assertEquals(-1, SwanStateFiles.slot("SLUS-00594-abc123-autosave.swanstate"))
        assertNull(SwanStateFiles.slot("x-10.swanstate"))
        assertNull(SwanStateFiles.slot("x-1.swanstate.png"))
    }
    @Test fun identityAndCompanions() {
        assertEquals("SLUS-00594", SwanStateFiles.identity("SLUS-00594-abcdef-3.swanstate"))
        assertEquals(listOf("x.png", "x.game"), SwanStateFiles.companions("x"))
    }
}
