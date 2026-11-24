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

    @Test fun warmListKeepsCustomFilesAndUrlsInDisplayOrder() {
        val custom = java.io.File("/data/covers/My Game.png")
        val models = listOf<Any>(custom, "https://example.invalid/a.jpg", "https://example.invalid/a.jpg", 42)
        val decoded = CoverSourcePolicy.decodeWarmList(CoverSourcePolicy.encodeWarmList(models))
        assertEquals(listOf<Any>(custom, "https://example.invalid/a.jpg"), decoded)
        assertTrue(decoded[0] is java.io.File)
    }

    @Test fun warmListIgnoresEmptyAndMalformedLines() {
        assertEquals(emptyList<Any>(), CoverSourcePolicy.decodeWarmList(""))
        assertEquals(listOf<Any>("b"), CoverSourcePolicy.decodeWarmList("junk\nS\t\nS\tb\nX\tc"))
    }
}
