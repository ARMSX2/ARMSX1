package com.armsx2.cheats

import android.content.Context
import android.util.AtomicFile
import java.io.File
import java.util.zip.ZipInputStream

/** Offline-first catalogue. Updates replace only the database, never user cheat files. Read on IO. */
object Ps1CheatDatabase {
    @Volatile private var cachedIndex: Ps1CheatDatabaseIndex? = null
    private var downloaded: Ps1CheatUpdatePackage? = null
    private var checkedDownload = false
    private var cacheDirectory: String? = null

    private fun updateFile(context: Context) = AtomicFile(File(context.filesDir, "cheat-database/update-v1.zip"))

    @Synchronized private fun checkDownload(context: Context) {
        if (cacheDirectory != context.filesDir.absolutePath) {
            cacheDirectory = context.filesDir.absolutePath
            downloaded = null
            cachedIndex = null
            checkedDownload = false
        }
        if (checkedDownload) return
        downloaded = runCatching {
            updateFile(context).openRead().use {
                Ps1CheatUpdatePackage.parse(Ps1CheatUpdatePackage.readBounded(it, Ps1CheatUpdatePackage.MAX_DOWNLOAD))
            }
        }.getOrNull() // Missing/corrupt downloads always fall back to the bundled catalogue.
        checkedDownload = true
    }

    /** Validation finishes before the atomic write. A failure leaves the previous catalogue intact. */
    fun installUpdate(context: Context, bytes: ByteArray): Boolean {
        val candidate = Ps1CheatUpdatePackage.parse(bytes)
        synchronized(this) {
            checkDownload(context)
            if (downloaded?.digest == candidate.digest) return false
            val atomic = updateFile(context)
            val output = atomic.startWrite()
            try {
                output.write(bytes)
                atomic.finishWrite(output)
                // AtomicFile logs some rename/sync failures instead of throwing them.
                check(atomic.openRead().use {
                    Ps1CheatUpdatePackage.sha256(Ps1CheatUpdatePackage.readBounded(it, Ps1CheatUpdatePackage.MAX_DOWNLOAD))
                } == candidate.digest) { "Unable to persist the cheat update" }
            } catch (failure: Throwable) {
                atomic.failWrite(output)
                throw failure
            }
            downloaded = candidate
            cachedIndex = candidate.index
            return true
        }
    }

    @Synchronized fun downloadedRevision(context: Context): String? {
        checkDownload(context)
        return downloaded?.digest?.take(12)
    }

    @Synchronized fun notice(context: Context): String {
        checkDownload(context)
        return downloaded?.notice ?: context.assets.open("ps1-cheats-NOTICE.txt").bufferedReader().use { it.readText() }
    }

    @Synchronized fun index(context: Context): Ps1CheatDatabaseIndex {
        checkDownload(context)
        downloaded?.let { return it.index }
        cachedIndex?.let { return it }
        return context.assets.open("ps1-cheats-index.tsv").bufferedReader().use { index ->
            context.assets.open("ps1-cover-titles.tsv").bufferedReader().use { metadata ->
                Ps1CheatDatabaseIndex(index, metadata)
            }
        }.also { cachedIndex = it }
    }

    @Synchronized fun catalogue(context: Context, item: Ps1CheatDatabaseItem): String {
        require(item.id.matches(Regex("[a-f0-9]{20}")))
        checkDownload(context)
        downloaded?.let { return it.bodies[item.id] ?: error("Cheat list changed; reload the database") }
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
    @Synchronized fun catalogues(context: Context, items: List<Ps1CheatDatabaseItem>): List<Pair<String, String>> {
        if (items.isEmpty()) return emptyList()
        require(items.all { it.id.matches(Regex("[a-f0-9]{20}")) })
        checkDownload(context)
        downloaded?.let { update ->
            return items.map { Ps1CheatDatabaseIndex.formatLabel(it.title) to
                (update.bodies[it.id] ?: error("Cheat list changed; reload the database")) }
        }
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
