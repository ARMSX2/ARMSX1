package com.armsx2.core

internal object Ps1MemoryCardImage {
    fun formatted(): ByteArray {
        val image = ByteArray(128 * 1024) { 0xff.toByte() }
        image.fill(0, 0, 128)
        image[0] = 'M'.code.toByte()
        image[1] = 'C'.code.toByte()
        image[127] = ('M'.code xor 'C'.code).toByte()
        for (sector in 1 until 36) {
            val start = sector * 128
            image.fill(0, start, start + 128)
            if (sector < 16) image[start] = 0xa0.toByte()
            else image.fill(0xff.toByte(), start, start + 4)
            image[start + 8] = 0xff.toByte()
            image[start + 9] = 0xff.toByte()
            var checksum = 0
            for (i in start until start + 127) checksum = checksum xor image[i].toInt()
            image[start + 127] = checksum.toByte()
        }
        image.copyInto(image, 63 * 128, 0, 128)
        return image
    }
}
