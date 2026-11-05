package com.armsx2.ui.home

import org.junit.Assert.assertEquals
import org.junit.Test

class HomeScrollPolicyTest {
    @Test fun hiddenCollectionsDoNotShiftTheFirstOrLastGame() {
        val leading = HomeScrollPolicy.leadingItems(false, true, false, true, true)
        assertEquals(4, leading)
        assertEquals(4, HomeScrollPolicy.targetIndex(0, leading, 1))
        assertEquals(103, HomeScrollPolicy.targetIndex(99, leading, 1))
        assertEquals(5, HomeScrollPolicy.leadingItems(false, true, true, true, true))
    }

    @Test fun bottomToolbarAndHiddenChromeDoNotAddPhantomItems() {
        assertEquals(0, HomeScrollPolicy.leadingItems(true, false, false, false, false))
        assertEquals(1, HomeScrollPolicy.leadingItems(false, false, false, false, false))
        assertEquals(2, HomeScrollPolicy.leadingItems(true, true, false, true, false))
    }

    @Test fun shelfSelectionFollowsTheRowInBothDirections() {
        assertEquals(4, HomeScrollPolicy.targetIndex(11, 3, 6))
        assertEquals(5, HomeScrollPolicy.targetIndex(12, 3, 6))
        assertEquals(4, HomeScrollPolicy.targetIndex(6, 3, 6))
        assertEquals(3, HomeScrollPolicy.targetIndex(5, 3, 6))
    }

    @Test fun revealsClippedItemsAtEitherEdgeForRowsAndColumns() {
        // The same axis math drives left/right lists and up/down grids.
        assertEquals(-40, HomeScrollPolicy.revealDelta(-20, 100, 20, 420))
        assertEquals(60, HomeScrollPolicy.revealDelta(380, 100, 20, 420))
        assertEquals(0, HomeScrollPolicy.revealDelta(20, 100, 20, 420))
        assertEquals(0, HomeScrollPolicy.revealDelta(320, 100, 20, 420))
    }

    @Test fun tallRowsDoNotBounceBetweenTheirEdges() {
        assertEquals(0, HomeScrollPolicy.revealDelta(-100, 600, 20, 420))
        assertEquals(40, HomeScrollPolicy.revealDelta(60, 600, 20, 420))
        assertEquals(-40, HomeScrollPolicy.revealDelta(-220, 600, 20, 420))
    }
}
