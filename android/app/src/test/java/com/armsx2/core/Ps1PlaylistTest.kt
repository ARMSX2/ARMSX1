package com.armsx2.core

import java.nio.file.Files
import java.io.File
import org.junit.Assert.*
import org.junit.Test

class Ps1PlaylistTest {
    private fun withGame(test: (File) -> Unit) {
        val root = Files.createTempDirectory("armsx-playlist-test").toFile()
        try { test(root) } finally { root.deleteRecursively() }
    }

    @Test fun directDiscFindsItsPlaylistAndRetainsMissingEntries() = withGame { root ->
        val first = File(root, "Game (Disc 1).chd").apply { writeText("fixture") }
        File(root, "Game (Disc 2).chd").writeText("fixture")
        File(root, "Game.m3u").writeText("\uFEFF#EXTM3U\n\"Game (Disc 1).chd\"\nGame (Disc 2).chd\nGame (Disc 3).chd")
        val discs = Ps1Playlist.discsForImage(first)
        assertEquals(3, discs.size)
        assertEquals("Disc 2", discs[1].label)
        assertTrue(discs[1].exists)
        assertFalse(discs[2].exists)
        assertEquals(first.absolutePath, discs[0].path)
    }

    @Test fun numberedImagesWorkWithoutAPlaylistAndDoNotIncludeOtherGames() = withGame { root ->
        val first = File(root, "Game (Disc 1).cue").apply { writeText("fixture") }
        listOf("Game (Disc 2).cue", "Game (Disc 2).bin", "Other (Disc 2).cue")
            .forEach { File(root, it).writeText("fixture") }
        assertEquals(listOf("Game (Disc 1).cue", "Game (Disc 2).cue"),
            Ps1Playlist.discsForImage(first).map { it.file.name })
    }

    @Test fun relativeSubfoldersAndWindowsSeparatorsResolve() = withGame { root ->
        val directory = File(root, "Disc 1").apply { mkdir() }
        val image = File(directory, "game.chd").apply { writeText("fixture") }
        val playlist = File(root, "game.m3u").apply { writeText("Disc 1\\game.chd") }
        assertEquals(image.canonicalFile, Ps1Playlist.bootDisc(playlist)?.canonicalFile)
        assertEquals(image.canonicalFile, Ps1Playlist.discsForImage(image).single().file.canonicalFile)
    }

    @Test fun conflictingPlaylistsAreNotGuessed() = withGame { root ->
        val image = File(root, "game.chd").apply { writeText("fixture") }
        File(root, "a.m3u").writeText("game.chd\nother.chd")
        File(root, "b.m3u").writeText("game.chd\ndifferent.chd")
        assertTrue(Ps1Playlist.discsForImage(image).isEmpty())
    }

    @Test fun labelsSortNumericallyAndSingleDiscsDoNotInventASeries() {
        assertEquals(listOf("Game CD1.chd", "Game CD2.chd", "Game CD10.chd"),
            Ps1Playlist.siblingDiscNames("Game CD1.chd", listOf("Game CD10.chd", "Game CD2.chd", "Game CD1.chd")))
        assertTrue(Ps1Playlist.siblingDiscNames("game.chd", listOf("game.chd", "other.chd")).isEmpty())
    }
}
