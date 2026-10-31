package com.armsx2.data.library

import androidx.compose.runtime.mutableStateOf
import com.armsx2.runtime.MainActivityRuntime
import org.json.JSONArray
import org.json.JSONObject
import java.util.UUID

/** Shared by the launcher and the in-session library. Never scans, moves or deletes ROMs. */
object GameCollections {
    private const val Key = "library.customCollections.v1"
    private const val SelectionKey = "library.customCollection.selected"
    private const val HomeOrderKey = "library.customCollections.homeOrder.v1"
    private var loaded = false
    val items = mutableStateOf<List<GameCollection>>(emptyList())
    val selectedId = mutableStateOf<String?>(null)
    val readFailed = mutableStateOf(false)
    val homeOrder = mutableStateOf<List<String>>(emptyList())

    fun ensureLoaded() {
        if (loaded) return
        val raw = MainActivityRuntime.prefs.getString(Key, "[]") ?: "[]"
        val result = runCatching {
            val array = JSONArray(raw)
            List(array.length()) { index ->
                val entry = array.getJSONObject(index)
                val uris = entry.optJSONArray("games") ?: JSONArray()
                GameCollection(entry.getString("id"), entry.getString("name"),
                    List(uris.length()) { uris.getString(it) }.toSet(), entry.optBoolean("onHome", false))
            }.filter { it.id.isNotBlank() && it.name.isNotBlank() }.distinctBy { it.id }
        }
        // A read failure must not quietly erase existing collections on the next edit.
        result.onFailure { android.util.Log.e("ARMSX-Collections", "Unable to read collections", it) }
        readFailed.value = result.isFailure
        items.value = result.getOrDefault(emptyList())
        selectedId.value = MainActivityRuntime.prefs.getString(SelectionKey, null)
            ?.takeIf { id -> items.value.any { it.id == id } }
        homeOrder.value = runCatching {
            val array = JSONArray(MainActivityRuntime.prefs.getString(HomeOrderKey, "[]") ?: "[]")
            List(array.length()) { array.getString(it) }
        }.getOrDefault(emptyList())
        loaded = true
    }

    fun select(id: String?) {
        ensureLoaded()
        selectedId.value = id?.takeIf { candidate -> items.value.any { it.id == candidate } }
        MainActivityRuntime.prefs.edit().putString(SelectionKey, selectedId.value).apply()
    }

    fun create(name: String, initialUri: String? = null): GameCollection {
        ensureLoaded()
        require(CollectionPolicy.validName(items.value, name))
        val collection = GameCollection(UUID.randomUUID().toString(), name.trim(),
            initialUri?.let { setOf(it) } ?: emptySet())
        save(items.value + collection)
        return collection
    }

    fun rename(id: String, name: String) = save(CollectionPolicy.rename(items.value, id, name))
    fun setMember(id: String, uri: String, member: Boolean) =
        save(CollectionPolicy.setMember(items.value, id, uri, member))
    fun setOnHome(id: String, onHome: Boolean) = save(CollectionPolicy.setOnHome(items.value, id, onHome))

    fun homeTileKeys() = CollectionPolicy.homeTileKeys(items.value, homeOrder.value)
    fun canMoveHomeTile(id: String, direction: Int): Boolean {
        val keys = homeTileKeys()
        return CollectionPolicy.moveHomeTile(keys, id, direction) != keys
    }
    fun moveHomeTile(id: String, direction: Int) {
        ensureLoaded()
        val keys = homeTileKeys()
        val moved = CollectionPolicy.moveHomeTile(keys, id, direction)
        if (keys == moved) return
        MainActivityRuntime.prefs.edit().putString(HomeOrderKey, JSONArray(moved).toString()).apply()
        homeOrder.value = moved
    }

    fun delete(id: String) {
        save(items.value.filterNot { it.id == id })
        if (selectedId.value == id) select(null)
    }

    fun includes(uri: String) = CollectionPolicy.includes(items.value, selectedId.value, uri)

    private fun save(next: List<GameCollection>) {
        check(!readFailed.value) { "Existing collections could not be read; refusing to overwrite them." }
        val array = JSONArray()
        next.forEach { collection ->
            array.put(JSONObject().put("id", collection.id).put("name", collection.name)
                .put("games", JSONArray(collection.gameUris.toList())).put("onHome", collection.onHome))
        }
        MainActivityRuntime.prefs.edit().putString(Key, array.toString()).apply()
        items.value = next
    }
}
