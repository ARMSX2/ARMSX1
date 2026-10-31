package com.armsx2.data.library

/** Membership uses the cached document URI, so rescans and title changes keep each disc's groups. */
data class GameCollection(val id: String, val name: String, val gameUris: Set<String> = emptySet(),
    val onHome: Boolean = false)

object CollectionPolicy {
    const val HomeFolderKey = "collections-browser"

    fun homeTileKeys(collections: List<GameCollection>, savedOrder: List<String>): List<String> {
        val available = listOf(HomeFolderKey) + collections.filter { it.onHome }.map { it.id }
        return (savedOrder.filter { it in available } + available).distinct()
    }

    fun moveHomeTile(keys: List<String>, id: String, direction: Int): List<String> {
        val index = keys.indexOf(id)
        val target = index + direction.coerceIn(-1, 1)
        if (index < 0 || target !in keys.indices || index == target) return keys
        return keys.toMutableList().apply { this[index] = keys[target]; this[target] = id }
    }

    fun validName(collections: List<GameCollection>, name: String, exceptId: String? = null): Boolean =
        name.trim().isNotEmpty() && collections.none {
            it.id != exceptId && it.name.equals(name.trim(), ignoreCase = true)
        }

    fun rename(collections: List<GameCollection>, id: String, name: String): List<GameCollection> {
        require(validName(collections, name, id))
        return collections.map { if (it.id == id) it.copy(name = name.trim()) else it }
    }

    fun setMember(collections: List<GameCollection>, id: String, uri: String, member: Boolean) =
        collections.map {
            if (it.id != id) it else it.copy(gameUris = if (member) it.gameUris + uri else it.gameUris - uri)
        }

    fun selected(collections: List<GameCollection>, id: String?) = collections.find { it.id == id }
    fun setOnHome(collections: List<GameCollection>, id: String, onHome: Boolean) =
        collections.map { if (it.id == id) it.copy(onHome = onHome) else it }
    fun includes(collections: List<GameCollection>, id: String?, uri: String): Boolean =
        selected(collections, id)?.gameUris?.contains(uri) ?: true
}
