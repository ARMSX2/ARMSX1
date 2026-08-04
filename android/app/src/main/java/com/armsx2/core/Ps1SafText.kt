package com.armsx2.core

import java.util.Locale

/** Pure parsers shared by SAF discovery, launch preparation, and host-side unit tests. */
internal object Ps1SafText {
    private val safeExtension = Regex("^[a-z0-9]{1,8}$")
    private val cueFile = Regex(
        pattern = "(?im)^(\\s*FILE\\s+)(?:\"([^\"\\r\\n]+)\"|([^\\s\\r\\n]+))(\\s+.*)$",
    )

    fun cueReferences(text: String): List<String> = cueFile.findAll(text).mapNotNull { match ->
        (match.groups[2]?.value ?: match.groups[3]?.value)?.trim()?.takeIf(String::isNotEmpty)
    }.toList()

    fun rewriteCue(text: String, localNames: List<String>): String {
        var index = 0
        val rewritten = cueFile.replace(text) { match ->
            val local = localNames.getOrNull(index++) ?: return@replace match.value
            "${match.groupValues[1]}\"$local\"${match.groupValues[4]}"
        }
        require(index == localNames.size) {
            "CUE reference count changed while preparing the launch"
        }
        return rewritten
    }

    fun playlistEntries(text: String): List<String> = text.lineSequence()
        .map(String::trim)
        .filter { it.isNotEmpty() && !it.startsWith("#") }
        .toList()

    fun baseName(reference: String): String =
        reference.replace('\\', '/').substringAfterLast('/').trim()

    fun extension(name: String): String {
        val candidate = name.substringAfterLast('.', "").lowercase(Locale.US)
        // Provider display names are untrusted input. Only a compact filename extension may be
        // appended to the private descriptor-link name; separators or traversal text must never
        // become part of a path below the launch directory.
        return candidate.takeIf(safeExtension::matches).orEmpty()
    }
}
