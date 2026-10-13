package com.armsx2.ui.settings

import org.junit.Assert.*
import org.junit.Test

class Ps1OverrideNoticePolicyTest {
    @Test fun cheatOnlySelectionDoesNotProduceAnOverrideNotice() {
        assertTrue(Ps1OverrideNoticePolicy.visibleOverrides(mapOf(
            "cheats.enabled" to "true", "cheats.file" to "game.cht",
            "cheats.enabled_codes" to "[Lives]",
        )).isEmpty())
    }

    @Test fun otherOverridesRemainVisibleAndResetTargetsExcludeCheats() {
        val original = linkedMapOf("cheats.enabled" to "true", "video.internal_scale" to "3",
            "audio.volume" to "80", "cheats.enabled_codes" to "[Lives]")
        val visible = Ps1OverrideNoticePolicy.visibleOverrides(original)
        assertEquals(setOf("video.internal_scale", "audio.volume"), visible.keys)
        val remaining = original.filterKeys { it !in visible.keys }
        assertEquals(setOf("cheats.enabled", "cheats.enabled_codes"), remaining.keys)
        assertEquals(4, original.size) // Filtering the notice never mutates stored data.
    }

    @Test fun exactCheatsSectionIsExcludedNotSimilarlyNamedKeys() {
        assertTrue(Ps1OverrideNoticePolicy.isCheatSetting("cheats.file"))
        assertFalse(Ps1OverrideNoticePolicy.isCheatSetting("runtime.cheats"))
        assertFalse(Ps1OverrideNoticePolicy.isCheatSetting("cheats_extra.setting"))
    }
}
