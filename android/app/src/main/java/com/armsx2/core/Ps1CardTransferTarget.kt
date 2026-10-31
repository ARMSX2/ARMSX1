package com.armsx2.core

import java.io.File
import java.io.IOException

/** A file picker result belongs to the card selected before leaving the app. */
internal object Ps1CardTransferTarget {
    fun check(slot: Int, expectedPath: String, current: File) {
        if (slot !in 1..2 || expectedPath.isBlank() || File(expectedPath).canonicalFile != current.canonicalFile)
            throw IOException("The active card changed while the file picker was open. Please select the desired slot again.")
    }
}
