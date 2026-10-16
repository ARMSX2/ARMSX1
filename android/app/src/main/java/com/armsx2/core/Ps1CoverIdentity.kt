package com.armsx2.core

import java.util.Locale

/** Artwork identifiers only. Never changes disc/save/cheat/achievement identity. */
object Ps1CoverIdentity {
    private val serialPattern = Regex("^([A-Za-z]{4})[ _-]?(\\d{3})\\.?(\\d{2})$")
    fun serial(value: String?): String? = value?.trim()?.let { raw ->
        serialPattern.matchEntire(raw)?.let {
            "${it.groupValues[1].uppercase(Locale.US)}-${it.groupValues[2]}${it.groupValues[3]}"
        }
    }

    fun urls(value: String, threeD: Boolean): List<String> {
        val id = serial(value) ?: return emptyList()
        val base = "https://raw.githubusercontent.com/xlenore/psx-covers/main/covers"
        val flat = "$base/default/$id.jpg"
        val case = "$base/3d/$id.png"
        return if (threeD) listOf(case, flat) else listOf(flat, case)
    }
}
