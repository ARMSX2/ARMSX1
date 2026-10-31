// SPDX-License-Identifier: GPL-3.0-or-later
package com.armsx2.core

/** Format-specific discovery: an old-core state must never be loaded as a SwanStation state. */
object SwanStateFiles {
    private val slotPattern = Regex("-(autosave|[0-9])\\.swanstate$", RegexOption.IGNORE_CASE)
    private val identitySuffix = Regex("-[0-9a-f]{1,16}$", RegexOption.IGNORE_CASE)
    fun slot(name: String): Int? = slotPattern.find(name)?.groupValues?.get(1)?.let {
        if (it.equals("autosave", true)) -1 else it.toIntOrNull()
    }
    fun identity(name: String): String = name.replace(slotPattern, "").replace(identitySuffix, "")
    fun companions(name: String): List<String> = listOf(name + ".png", name + ".game")
}
