package com.armsx2.cheats

import android.content.Context
import java.util.zip.ZipInputStream

/** Bundled offline data. Read on Dispatchers.IO; never downloads while a game is running. */
object Ps1CheatDatabase {
    @Volatile private var cachedIndex: Ps1CheatDatabaseIndex? = null

    @Synchronized fun index(context: Context): Ps1CheatDatabaseIndex {
        cachedIndex?.let { return it }
        return context.assets.open("ps1-cheats-index.tsv").bufferedReader().use(::Ps1CheatDatabaseIndex)
            .also { cachedIndex = it }
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
}
