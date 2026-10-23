package com.armsx2.core

import java.io.File
import org.junit.Assert.*
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder

class Ps1CardBindingsTest {
    @get:Rule val temporary = TemporaryFolder()

    @Test fun selectedGameDoesNotUseTheLastGameCard() {
        val root = temporary.newFolder("cards")
        val a = File(root, "Game A_1.mcd")
        val b = File(root, "Game B_1.mcd")
        val shared = File(root, "duckstation_shared_card_2.mcd")
        Ps1CardBindings.remember(root, "content://roms/game-a.chd", listOf(a, shared))
        Ps1CardBindings.remember(root, "content://roms/game-b.chd", listOf(b, shared))
        assertEquals(a, Ps1CardBindings.resolve(root, "content://roms/game-a.chd", 1))
        assertEquals(b, Ps1CardBindings.resolve(root, "content://roms/game-b.chd", 1))
        assertEquals(shared, Ps1CardBindings.resolve(root, "content://roms/game-a.chd", 2))
    }

    @Test fun aGameNeverBootedDoesNotFallBackToSomeoneElsesCard() {
        val root = temporary.newFolder("cards")
        Ps1CardBindings.remember(root, "known", listOf(File(root, "Known_1.mcd"), File(root, "shared.mcd")))
        assertNull(Ps1CardBindings.resolve(root, "unknown", 1))
    }

    @Test fun differentRomLocationsRemainSeparateWithoutEditingTheirCards() {
        val root = temporary.newFolder("cards")
        val first = File(root, "First_1.mcd").apply { writeBytes(byteArrayOf(1, 2, 3)) }
        val second = File(root, "Second_1.mcd").apply { writeBytes(byteArrayOf(4, 5, 6)) }
        Ps1CardBindings.remember(root, "content://one/game.chd", listOf(first, second))
        assertNull(Ps1CardBindings.resolve(root, "content://two/game.chd", 1))
        assertArrayEquals(byteArrayOf(1, 2, 3), first.readBytes())
        assertArrayEquals(byteArrayOf(4, 5, 6), second.readBytes())
    }

    @Test fun bindingsSurviveReloadAndCanFollowTheCoresUpdatedPath() {
        val root = temporary.newFolder("cards")
        val old = File(root, "Old_1.mcd")
        val next = File(root, "New_1.mcd")
        val shared = File(root, "shared.mcd")
        Ps1CardBindings.remember(root, "game", listOf(old, shared))
        assertEquals(old, Ps1CardBindings.resolve(File(root.absolutePath), "game", 1))
        Ps1CardBindings.remember(root, "game", listOf(next, shared))
        assertEquals(next, Ps1CardBindings.resolve(root, "game", 1))
        assertFalse(root.listFiles()!!.any { it.name.endsWith(".tmp") })
    }

    @Test fun dataFolderMoveKeepsRelativeCardBindings() {
        val root = temporary.newFolder("original")
        val moved = temporary.newFolder("moved")
        Ps1CardBindings.remember(root, "game", listOf(File(root, "Game_1.mcd"), File(root, "shared.mcd")))
        root.listFiles()!!.forEach { it.copyTo(File(moved, it.name)) }
        assertEquals(File(moved, "Game_1.mcd"), Ps1CardBindings.resolve(moved, "game", 1))
    }

    @Test fun corruptMetadataAndTraversalCannotRedirectImports() {
        val root = temporary.newFolder("cards")
        assertNull(Ps1CardBindings.safeCard(root, "../outside.mcd"))
        assertNull(Ps1CardBindings.safeCard(root, ""))
        assertNull(Ps1CardBindings.safeCard(root, "save.swanstate"))
        Ps1CardBindings.remember(root, "game", listOf(File(root, "Game.mcd"), File(root, "shared.mcd")))
        root.listFiles()!!.single { it.name.endsWith(".cards") }.writeText("invalid json")
        assertNull(Ps1CardBindings.resolve(root, "game", 1))
    }

    @Test fun bulkBindingsKeepOtherSlotsAndOriginalCardsUntouched() {
        val root = temporary.newFolder("bulk")
        val original = File(root, "Original.mcd").apply { writeText("old") }
        val imported = File(root, "Imported.mcd").apply { writeText("new") }
        val second = File(root, "Second.mcd").apply { writeText("second") }
        Ps1CardBindings.remember(root, "game", listOf(original, second))
        Ps1CardBindings.rememberImported(root, "game", mapOf(1 to imported))
        assertEquals(imported, Ps1CardBindings.imported(root, "game", 1))
        assertNull(Ps1CardBindings.imported(root, "game", 2))
        assertNull(Ps1CardBindings.imported(root, "different game", 1))
        Ps1CardBindings.rememberImported(root, "game", mapOf(2 to second))
        assertEquals(imported, Ps1CardBindings.imported(root, "game", 1))
        assertEquals(second, Ps1CardBindings.imported(root, "game", 2))
        assertEquals(original, Ps1CardBindings.resolve(root, "game", 1))
        assertEquals("old", original.readText())
    }
}
