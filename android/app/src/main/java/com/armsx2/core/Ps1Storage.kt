package com.armsx2.core

import android.content.Context
import android.content.Intent
import android.net.Uri
import android.os.Build
import android.os.Environment
import android.provider.DocumentsContract
import android.provider.Settings
import androidx.core.net.toUri

/**
 * All-files storage access + folder-URI → absolute-path resolution.
 *
 * The PS1 native core opens games by absolute filesystem path (no SAF), so to let users point at
 * their existing ROM folders we need MANAGE_EXTERNAL_STORAGE ("all files access"). ARMSX is
 * sideloaded and not Play/GPL-constrained, so this is the github-flavour approach (same as ARMSX2's
 * all-files build). A SAF folder pick (ACTION_OPEN_DOCUMENT_TREE) yields a tree URI we convert to a
 * `/storage/...` path the core can fopen.
 */
object Ps1Storage {

    /** True when the app holds MANAGE_EXTERNAL_STORAGE (all files access). Pre-R: always true. */
    fun hasAllFilesAccess(): Boolean =
        Build.VERSION.SDK_INT < Build.VERSION_CODES.R || Environment.isExternalStorageManager()

    /** Intent that opens the system "Allow access to manage all files" screen for this app. */
    fun allFilesAccessIntent(context: Context): Intent =
        Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION, "package:${context.packageName}".toUri())

    /** Intent to pick a folder (SAF tree). We only use the returned tree URI to derive a POSIX path. */
    fun pickFolderIntent(): Intent =
        Intent(Intent.ACTION_OPEN_DOCUMENT_TREE).addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION)

    /**
     * Convert a SAF tree URI to an absolute `/storage/...` path. `primary:` volumes map to
     * `/storage/emulated/0/<rel>`; other volume ids (SD cards) to `/storage/<vol>/<rel>`. Lifted
     * from ARMSX2's resolveTreeUriToPosix.
     */
    fun resolveTreeUriToPosix(uriString: String?): String? {
        val raw = uriString ?: return null
        val uri: Uri = try { raw.toUri() } catch (_: Exception) { return null }
        val docId = try { DocumentsContract.getTreeDocumentId(uri) } catch (_: Exception) { null } ?: return null
        val parts = docId.split(":", limit = 2)
        if (parts.size != 2) return null
        val (volumeId, relPath) = parts
        return when (volumeId) {
            "primary" -> "/storage/emulated/0/$relPath"
            else -> "/storage/$volumeId/$relPath"
        }
    }
}
