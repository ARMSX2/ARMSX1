package com.armsx2.data.library

import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertThrows
import org.junit.Assert.assertTrue
import org.junit.Test

class RecentGamesQueryTest {
    @Test
    fun acceptsOnlyGamesInOwnAuthority() {
        val pkg = "com.nanodata.armsx"
        assertTrue(RecentGamesQuery.matches("content", "$pkg.romlibrary", "/games", pkg))
        assertFalse(RecentGamesQuery.matches("file", "$pkg.romlibrary", "/games", pkg))
        assertFalse(RecentGamesQuery.matches("content", "other.romlibrary", "/games", pkg))
        for (path in listOf(null, "", "/", "/games/1", "/other")) {
            assertFalse(RecentGamesQuery.matches("content", "$pkg.romlibrary", path, pkg))
        }
    }

    @Test
    fun defaultProjectionAndRequestedOrder() {
        assertArrayEquals(
            arrayOf("uri", "title", "serial", "ext", "platform", "lastPlayed"),
            RecentGamesQuery.columns(null),
        )
        assertArrayEquals(arrayOf("serial", "title"), RecentGamesQuery.columns(arrayOf("serial", "title")))
        assertArrayEquals(emptyArray<String>(), RecentGamesQuery.columns(emptyArray()))
    }

    @Test
    fun rejectsUnknownColumns() {
        assertThrows(IllegalArgumentException::class.java) {
            RecentGamesQuery.columns(arrayOf("title", "private"))
        }
    }

    @Test
    fun rejectsUnsupportedFilteringAndSorting() {
        RecentGamesQuery.validateArguments(null, null, null)
        RecentGamesQuery.validateArguments("", emptyArray(), "")
        assertThrows(IllegalArgumentException::class.java) {
            RecentGamesQuery.validateArguments("serial = ?", arrayOf("SCES-02031"), null)
        }
        assertThrows(IllegalArgumentException::class.java) {
            RecentGamesQuery.validateArguments(null, arrayOf("SCES-02031"), null)
        }
        assertThrows(IllegalArgumentException::class.java) {
            RecentGamesQuery.validateArguments(null, null, "lastPlayed DESC")
        }
    }
}
