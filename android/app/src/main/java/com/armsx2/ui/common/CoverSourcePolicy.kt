package com.armsx2.ui.common

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
}
