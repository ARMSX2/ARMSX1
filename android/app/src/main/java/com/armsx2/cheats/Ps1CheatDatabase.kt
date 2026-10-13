package com.armsx2.cheats

import android.content.Context
import java.util.zip.ZipInputStream

/** Bundled offline data. Read on Dispatchers.IO; never downloads while a game is running. */
object Ps1CheatDatabase {
    @Volatile private var cachedIndex: Ps1CheatDatabaseIndex? = null

    @Synchronized fun index(context: Context): Ps1CheatDatabaseIndex {
        cachedIndex?.let { return it }
        return context.assets.open("ps1-cheats-index.tsv").bufferedReader().use { index ->
            context.assets.open("ps1-cover-titles.tsv").bufferedReader().use { metadata ->
                Ps1CheatDatabaseIndex(index, metadata)
            }
        }.also { cachedIndex = it }
    }

    fun catalogue(context: Context, item: Ps1CheatDatabaseItem): String {
        require(item.id.matches(Regex("[a-f0-9]{20}")))
        ZipInputStream(context.assets.open("ps1-cheats.zip")).use { archive ->
            while (true) {
                val entry = archive.nextEntry ?: break
                if (entry.name == item.id + ".cht") {
                    require(entry.size in 1..2_000_000)
                    return archive.bufferedReader(Charsets.UTF_8).readText()
                }
            }
        }
        error("Bundled cheat catalogue is missing")
    }

    /** Read all selected lists in one archive pass, then merge in stable index order. */
    fun catalogues(context: Context, items: List<Ps1CheatDatabaseItem>): List<Pair<String, String>> {
        if (items.isEmpty()) return emptyList()
        require(items.all { it.id.matches(Regex("[a-f0-9]{20}")) })
        val wanted = items.mapTo(hashSetOf()) { it.id + ".cht" }
        val bodies = mutableMapOf<String, String>()
        ZipInputStream(context.assets.open("ps1-cheats.zip")).use { archive ->
            while (bodies.size < wanted.size) {
                val entry = archive.nextEntry ?: break
                if (entry.name in wanted) {
                    require(entry.size in 1..2_000_000)
                    bodies[entry.name] = archive.bufferedReader(Charsets.UTF_8).readText()
                }
            }
        }
        check(bodies.size == wanted.size) { "Bundled cheat catalogue is missing" }
        return items.map { Ps1CheatDatabaseIndex.formatLabel(it.title) to bodies.getValue(it.id + ".cht") }
    }
}
