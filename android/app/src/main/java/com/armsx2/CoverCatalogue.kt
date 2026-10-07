package com.armsx2

import android.content.Context
import androidx.compose.runtime.mutableIntStateOf
import com.armsx2.core.Ps1TitleSerials

/** Load bundled artwork metadata on a worker, then refresh already-visible cover tiles. */
object CoverCatalogue {
    val version = mutableIntStateOf(0)
    @Volatile private var loaded = false

    @Synchronized fun load(context: Context) {
        if (loaded) return
        runCatching {
            context.assets.open("ps1-cover-titles.tsv").bufferedReader().use {
                Ps1TitleSerials.loadCatalogue(it)
            }
        }.onSuccess { count ->
            loaded = true
            version.intValue++
            println("@@ARMSX_COVER_CATALOGUE@@ entries=$count")
        }.onFailure { println("@@ARMSX_COVER_CATALOGUE@@ failed=${it.javaClass.simpleName}") }
    }
}
