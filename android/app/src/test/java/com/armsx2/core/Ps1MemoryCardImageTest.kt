package com.armsx2.core

import org.junit.Assert.assertEquals
import org.junit.Test

class Ps1MemoryCardImageTest {
    @Test
    fun newCardHasFormattedDirectoryAndValidChecksums() {
        val image = Ps1MemoryCardImage.formatted()
        assertEquals(131072, image.size)
        assertEquals('M'.code, image[0].toInt())
        assertEquals('C'.code, image[1].toInt())
        for (sector in 0 until 36) {
            var checksum = 0
            for (i in 0 until 128) checksum = checksum xor (image[sector * 128 + i].toInt() and 255)
            assertEquals(0, checksum)
            if (sector in 1 until 16) {
                assertEquals(0xa0, image[sector * 128].toInt() and 255)
                assertEquals(255, image[sector * 128 + 8].toInt() and 255)
                assertEquals(255, image[sector * 128 + 9].toInt() and 255)
            }
        }
        for (i in 0 until 128) assertEquals(image[i], image[63 * 128 + i])
    }
}
