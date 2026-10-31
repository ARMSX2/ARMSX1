package com.armsx2.ui.settings

import org.junit.After
import org.junit.Before
import org.junit.Assert.*
import org.junit.Test

class SettingsControllerNavTest {
    private val background = "outline.test.background"
    private val popup = "outline.test.popup"
    private val layer = "outline.test.modal"

    @Before fun setUp() {
        SettingsControllerNav.activeLayer.value = null
        SettingsControllerNav.clearSelection()
        SettingsControllerNav.register(background)
        SettingsControllerNav.register(popup, layer = layer)
    }

    @After fun tearDown() {
        SettingsControllerNav.activeLayer.value = null
        SettingsControllerNav.clearSelection()
        SettingsControllerNav.unregister(background)
        SettingsControllerNav.unregister(popup)
    }

    @Test fun openingPopupHidesBackgroundEvenBeforePopupSelectsAnItem() {
        assertTrue(SettingsControllerNav.selectById(background))
        assertTrue(SettingsControllerNav.isSelected(background))
        SettingsControllerNav.activeLayer.value = layer
        assertFalse(SettingsControllerNav.isSelected(background))
        assertFalse(SettingsControllerNav.hasSelection())
        assertTrue(SettingsControllerNav.selectById(popup))
        assertTrue(SettingsControllerNav.isSelected(popup))
        assertFalse(SettingsControllerNav.isSelected(background))
    }

    @Test fun closingPopupCannotLeaveItsOldControlHighlighted() {
        SettingsControllerNav.activeLayer.value = layer
        assertTrue(SettingsControllerNav.selectById(popup))
        SettingsControllerNav.activeLayer.value = null
        assertFalse(SettingsControllerNav.isSelected(popup))
        assertFalse(SettingsControllerNav.hasSelection())
        assertTrue(SettingsControllerNav.selectById(background))
        assertTrue(SettingsControllerNav.isSelected(background))
        SettingsControllerNav.clearSelection()
        assertFalse(SettingsControllerNav.isSelected(background))
    }

    @Test fun popupCloseReturnsSelectionAndAllowsRepeatedOpenClose() {
        assertTrue(SettingsControllerNav.selectById(background))
        repeat(3) {
            val close = SettingsControllerNav.claimLayer(layer)
            assertFalse(SettingsControllerNav.isSelected(background))
            assertTrue(SettingsControllerNav.selectById(popup))
            close()
            assertNull(SettingsControllerNav.activeLayer.value)
            assertTrue(SettingsControllerNav.isSelected(background))
            assertTrue(SettingsControllerNav.hasSelection())
            close() // Disposal is harmless if another close path already released it.
            assertTrue(SettingsControllerNav.isSelected(background))
        }
    }

    @Test fun closingPopupDoesNotSelectAnItemRemovedWhileItWasOpen() {
        assertTrue(SettingsControllerNav.selectById(background))
        val close = SettingsControllerNav.claimLayer(layer)
        SettingsControllerNav.unregister(background)
        close()
        assertNull(SettingsControllerNav.activeLayer.value)
        assertFalse(SettingsControllerNav.hasSelection())
    }
}
