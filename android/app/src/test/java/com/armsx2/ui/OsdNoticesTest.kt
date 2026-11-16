package com.armsx2.ui

import com.armsx2.i18n.EN
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test

class OsdNoticesTest {
    private val text: (String) -> String = { EN[it] ?: it }

    @Test
    fun memoryCardSavingAndSavedShareOneKeyPerCard() {
        val saving = OsdNotices.format("memory_card.saving", "1", text)!!
        val saved = OsdNotices.format("memory_card.saved", "1", text)!!
        assertEquals("Memory Card 1: saving…", saving.text)
        assertEquals("Memory Card 1 saved", saved.text)
        assertEquals(saving.key, saved.key)
        assertEquals("memory_card.2", OsdNotices.format("memory_card.saving", "2", text)!!.key)
    }

    @Test
    fun failureCarriesTheErrorAndBadSlotsAreIgnored() {
        assertEquals(
            "Memory Card 2 could not be saved: No space left on device",
            OsdNotices.format("memory_card.failed", "2\u001fNo space left on device", text)!!.text,
        )
        assertNull(OsdNotices.format("memory_card.saved", "3", text))
        assertNull(OsdNotices.format("memory_card.saved", "", text))
        assertNull(OsdNotices.format("unknown", "1", text))
    }

    @Test
    fun cheatsListFitsOneLineAndCountsTheRest() {
        assertEquals("Cheats off", OsdNotices.format("cheats", "", text)!!.text)
        assertEquals(
            "Cheats active (2): Infinite HP, Max Gil",
            OsdNotices.format("cheats", "Infinite HP\u001fMax Gil", text)!!.text,
        )
        val names = (1..12).map { "Cheat number $it" }
        val line = OsdNotices.format("cheats", names.joinToString("\u001f"), text)!!.text
        val shown = OsdNotices.cheatList(names, "+{count} more")
        assertEquals("Cheats active (12): $shown", line)
        val listed = shown.substringBefore(" +").split(", ").size
        assertEquals(" +${12 - listed} more", shown.substring(shown.indexOf(" +")))
    }

    @Test
    fun settingsSummaryIsShownAsSentAndBlankIsIgnored() {
        val summary = "2× HD Vulkan rendering · ARM64 recompiler · Fast boot on · Widescreen off"
        assertEquals(summary, OsdNotices.format("settings", summary, text)!!.text)
        assertNull(OsdNotices.format("settings", "  ", text))
    }
}
