// SPDX-License-Identifier: GPL-3.0-or-later
package com.armsx2.navigation

/** Controller Back goes to contents before leaving the reader, like Android/touch Back. */
internal object ManualReaderNavigation {
    var handler: (() -> Boolean)? = null
    fun back(): Boolean = handler?.invoke() == true
}
