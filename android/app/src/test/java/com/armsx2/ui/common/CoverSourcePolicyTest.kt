package com.armsx2.ui.common

import org.junit.Assert.*
import org.junit.Test

class CoverSourcePolicyTest {
    @Test fun previouslyWorkingFallbackLoadsBeforeTheMissingPreferredStyle() {
        val models = listOf("3d.png", "downloaded.jpg", "default.jpg")
        assertEquals(listOf(2, 0, 1), CoverSourcePolicy.order(models, "default.jpg"))
    }

    @Test fun changedStyleOrCustomCoverRetainsItsOwnPreference() {
        assertNotEquals(CoverSourcePolicy.key(listOf("3d", "2d")), CoverSourcePolicy.key(listOf("2d", "3d")))
        assertNotEquals(CoverSourcePolicy.key(listOf("3d", "2d")), CoverSourcePolicy.key(listOf("custom", "3d", "2d")))
        assertEquals(listOf(0, 1), CoverSourcePolicy.order(listOf("new-custom", "3d"), "old-custom"))
    }

    @Test fun sourcesWithSeparatorsCannotShareAStoredChoice() {
        assertNotEquals(CoverSourcePolicy.key(listOf("a:b", "c")), CoverSourcePolicy.key(listOf("a", "b:c")))
    }
}
