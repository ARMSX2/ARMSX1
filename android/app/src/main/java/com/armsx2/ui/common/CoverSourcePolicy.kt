package com.armsx2.ui.common

import java.io.File
import java.security.MessageDigest

internal object CoverSourcePolicy {
    fun key(sources: List<String>): String = MessageDigest.getInstance("SHA-256")
        .digest(sources.joinToString("") { "${it.length}:$it" }.toByteArray(Charsets.UTF_8))
        .joinToString("") { "%02x".format(it) }

    fun order(sources: List<String>, successful: String?): List<Int> {
        val preferred = sources.indexOf(successful)
        return if (preferred < 0) sources.indices.toList()
        else listOf(preferred) + sources.indices.filter { it != preferred }
    }

    /** Serialise the covers to decode at the next cold start, in display order, one per line. */
    fun encodeWarmList(models: List<Any>): String = models.mapNotNull { model ->
        when (model) {
            is File -> "F\t${model.path}"
            is String -> "S\t$model"
            else -> null
        }?.takeIf { '\n' !in it && '\r' !in it }
    }.distinct().joinToString("\n")

    fun decodeWarmList(text: String): List<Any> = text.lineSequence().mapNotNull { line ->
        val value = line.substringAfter('\t', "")
        when {
            value.isEmpty() -> null
            line.startsWith("F\t") -> File(value)
            line.startsWith("S\t") -> value
            else -> null
        }
    }.toList()
}
