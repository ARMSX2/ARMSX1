package com.armsx2.data.library

import android.content.Context
import android.content.Intent
import android.net.Uri
import androidx.compose.runtime.mutableIntStateOf
import com.armsx2.runtime.MainActivityRuntime

/** Artwork is keyed by stable folder ID, independently of its name and Home position. */
object FolderArtwork {
    private const val Prefix = "library.collectionArtwork."
    val version = mutableIntStateOf(0)

    fun image(id: String): String? = MainActivityRuntime.prefs.getString(Prefix + id, null)

    fun set(context: Context, id: String, uri: Uri) {
        context.contentResolver.takePersistableUriPermission(uri, Intent.FLAG_GRANT_READ_URI_PERMISSION)
        MainActivityRuntime.prefs.edit().putString(Prefix + id, uri.toString()).apply()
        version.intValue++
    }

    fun clear(id: String) {
        MainActivityRuntime.prefs.edit().remove(Prefix + id).apply()
        version.intValue++
    }
}
