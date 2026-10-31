package com.armsx2.cheats

/** Cheat-name prefixes narrow immediately; letters in the middle do not match. */
internal object Ps1CheatSearch {
    fun matches(name: String, query: String): Boolean =
        name.trimStart().startsWith(query.trim(), ignoreCase = true)
}
