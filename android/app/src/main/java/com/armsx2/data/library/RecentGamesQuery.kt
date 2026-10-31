package com.armsx2.data.library

internal object RecentGamesQuery {
    private val columns = listOf("uri", "title", "serial", "ext", "platform", "lastPlayed")

    fun matches(scheme: String?, authority: String?, path: String?, packageName: String): Boolean =
        scheme == "content" && authority == "$packageName.romlibrary" && path == "/games"

    fun columns(projection: Array<out String>?): Array<String> {
        val requested = projection?.toList() ?: columns
        require(requested.all { it in columns }) { "Unsupported recent games column" }
        return requested.toTypedArray()
    }

    fun validateArguments(selection: String?, selectionArgs: Array<out String>?, sortOrder: String?) {
        require(selection.isNullOrBlank() && selectionArgs.isNullOrEmpty()) {
            "Recent games filtering is not supported"
        }
        require(sortOrder.isNullOrBlank()) { "Recent games are returned in recently played order" }
    }
}
