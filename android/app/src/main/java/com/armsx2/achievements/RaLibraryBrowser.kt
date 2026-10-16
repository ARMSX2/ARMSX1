package com.armsx2.achievements

import java.util.Locale

/** Browser policy has no network/ROM access and never invents a match for an unsupported image. */
internal object RaLibraryBrowser {
    const val PAGE_SIZE = 20

    fun <T> select(games: List<T>, query: String, title: (T) -> String, serial: (T) -> String?, total: (T) -> Int): List<T> {
        val text = query.trim()
        return games.filter {
            total(it) > 0 && (text.isEmpty() || title(it).contains(text, true) || serial(it)?.contains(text, true) == true)
        }.sortedWith(compareBy<T> {
            if (text.isEmpty() || title(it).startsWith(text, true) || serial(it)?.startsWith(text, true) == true) 0 else 1
        }.thenBy { title(it).lowercase(Locale.ROOT) })
    }

    fun pageCount(count: Int): Int = maxOf(1, (count + PAGE_SIZE - 1) / PAGE_SIZE)
    fun safePage(page: Int, count: Int): Int = page.coerceIn(0, pageCount(count) - 1)
    fun <T> page(games: List<T>, page: Int): List<T> = games.drop(safePage(page, games.size) * PAGE_SIZE).take(PAGE_SIZE)
}
