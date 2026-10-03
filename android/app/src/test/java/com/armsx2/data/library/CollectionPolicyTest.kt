package com.armsx2.data.library

import org.junit.Assert.*
import org.junit.Test

class CollectionPolicyTest {
    private val disc = "content://roms/document/Crash%202.chd"
    private val otherDisc = "content://roms/document/Crash%202%20PAL.chd"
    private val groups = listOf(GameCollection("platform", "Platformers", setOf(disc)),
        GameCollection("favourite", "Favourites", setOf(disc, otherDisc)))

    @Test fun renameKeepsStableIdAndMembership() {
        val renamed = CollectionPolicy.rename(groups, "platform", "  Crash games  ")
        assertEquals("Crash games", renamed[0].name)
        assertEquals(groups[0].id, renamed[0].id)
        assertEquals(groups[0].gameUris, renamed[0].gameUris)
        assertEquals(groups[1], renamed[1])
    }

    @Test fun removingMembershipKeepsOtherGroupsAndSeparateDiscs() {
        val updated = CollectionPolicy.setMember(groups, "favourite", disc, false)
        assertTrue(CollectionPolicy.includes(updated, "platform", disc))
        assertFalse(CollectionPolicy.includes(updated, "favourite", disc))
        assertTrue(CollectionPolicy.includes(updated, "favourite", otherDisc))
    }

    @Test fun repeatedAddCannotDuplicateGameAndGameCanBelongToMultipleGroups() {
        val once = CollectionPolicy.setMember(groups, "platform", otherDisc, true)
        val twice = CollectionPolicy.setMember(once, "platform", otherDisc, true)
        assertEquals(once, twice)
        assertEquals(2, twice[0].gameUris.size)
        assertTrue(CollectionPolicy.includes(twice, "favourite", otherDisc))
    }

    @Test fun deletingSelectedGroupFallsBackToAllGames() {
        val remaining = groups.filterNot { it.id == "platform" }
        assertTrue(CollectionPolicy.includes(remaining, "platform", "newly-scanned-game"))
        assertEquals(groups[1], remaining.single())
    }

    @Test fun allGamesAndEmptyCollectionHaveDifferentMeaning() {
        val empty = listOf(GameCollection("empty", "To play"))
        assertTrue(CollectionPolicy.includes(empty, null, disc))
        assertFalse(CollectionPolicy.includes(empty, "empty", disc))
    }

    @Test fun namesRejectBlankAndCaseInsensitiveDuplicatesButAllowOwnRename() {
        assertFalse(CollectionPolicy.validName(groups, "   "))
        assertFalse(CollectionPolicy.validName(groups, " favourites "))
        assertTrue(CollectionPolicy.validName(groups, "PLATFORMERS", "platform"))
        assertFalse(CollectionPolicy.validName(groups, "Favourites", "platform"))
    }

    @Test fun homeShortcutSurvivesRenameAndMembershipChangesAndCanBeRemoved() {
        assertFalse(groups[0].onHome)
        val pinned = CollectionPolicy.setOnHome(groups, "platform", true)
        assertEquals(groups[0].id, pinned[0].id)
        assertEquals(groups[0].gameUris, pinned[0].gameUris)
        assertEquals(groups[1], pinned[1])
        val edited = CollectionPolicy.setMember(CollectionPolicy.rename(pinned, "platform", "Crash"),
            "platform", otherDisc, true)
        assertTrue(edited[0].onHome)
        val removed = CollectionPolicy.setOnHome(edited, "platform", false)
        assertFalse(removed[0].onHome)
        assertEquals(edited[0].gameUris, removed[0].gameUris)
        assertEquals("Crash", removed[0].name)
    }

    @Test fun homeOrderKeepsExistingTilesAndAddsNewPinsWithoutStaleOrDuplicateEntries() {
        val pinned = groups.map { it.copy(onHome = true) }
        assertEquals(listOf("favourite", CollectionPolicy.HomeFolderKey, "platform"),
            CollectionPolicy.homeTileKeys(pinned, listOf("deleted", "favourite", "favourite")))
        assertEquals(listOf(CollectionPolicy.HomeFolderKey), CollectionPolicy.homeTileKeys(groups, listOf("platform")))
    }

    @Test fun singleShortcutCanMoveBeforeTheMainFolderAndMovesAtEdgesAreSafe() {
        val keys = listOf(CollectionPolicy.HomeFolderKey, "platform")
        val moved = CollectionPolicy.moveHomeTile(keys, "platform", -1)
        assertEquals(listOf("platform", CollectionPolicy.HomeFolderKey), moved)
        assertEquals(moved, CollectionPolicy.moveHomeTile(moved, "platform", -1))
        assertEquals(keys, CollectionPolicy.moveHomeTile(moved, "platform", 1))
        assertEquals(keys, CollectionPolicy.moveHomeTile(keys, "missing", 1))
    }
}
