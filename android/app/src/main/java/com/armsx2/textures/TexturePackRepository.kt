package com.armsx2.textures

import android.content.Context
import android.util.AtomicFile
import com.armsx2.cheats.Ps1CheatUpdatePackage
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.currentCoroutineContext
import kotlinx.coroutines.ensureActive
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import kotlinx.coroutines.withContext
import java.io.File
import java.net.URI
import java.nio.file.Files
import java.nio.ByteBuffer
import java.nio.charset.CodingErrorAction
import javax.net.ssl.HttpsURLConnection

/** Immutable pack versions, staged installs and no changes to user-managed texture folders. */
object TexturePackRepository {
    const val CATALOGUE_URL = "https://raw.githubusercontent.com/ARMSX2/ARMSX1/main/texture-packs/catalogue.tsv"
    private val installLock = Mutex()
    private val catalogueLock = Mutex()
    private fun root(context: Context) = File(context.filesDir, "texture-packs").apply { check(isDirectory || mkdirs()) }
    private fun catalogue(context: Context) = AtomicFile(File(root(context), "catalogue.tsv"))
    private fun utf8(bytes: ByteArray) = Charsets.UTF_8.newDecoder().onMalformedInput(CodingErrorAction.REPORT)
        .onUnmappableCharacter(CodingErrorAction.REPORT).decode(ByteBuffer.wrap(bytes)).toString()

    fun directory(context: Context, pack: TexturePack): File {
        require(pack.id.matches(Regex("[a-z0-9][a-z0-9-]{0,63}")) && pack.sha256.matches(Regex("[a-f0-9]{64}")))
        return File(File(File(root(context), "packs"), pack.id), pack.sha256)
    }

    fun installed(context: Context, pack: TexturePack): Boolean {
        val directory = directory(context, pack)
        return runCatching {
            val receipt = File(directory, "pack.tsv")
            require(receipt.length() in 1..16_384)
            TexturePackCatalogue.parse(receipt.readText()).single().storageKey == pack.storageKey &&
                File(directory, "replacements").listFiles()?.any { it.isFile && it.length() > 33 } == true
        }.getOrDefault(false)
    }

    /** Cached catalogue plus locally installed versions, so removed remote entries remain usable. */
    fun load(context: Context): List<TexturePack> {
        val remote = runCatching {
            catalogue(context).openRead().use { TexturePackCatalogue.parse(utf8(Ps1CheatUpdatePackage.readBounded(it, 1024 * 1024))) }
        }.getOrElse {
            context.assets.open("ps1-texture-packs.tsv").bufferedReader().use { TexturePackCatalogue.parse(it.readText()) }
        }
        val local = File(root(context), "packs").listFiles().orEmpty().filter { it.isDirectory }.take(2000).flatMap { id ->
            id.listFiles().orEmpty().filter { it.isDirectory }.take(100).mapNotNull { version ->
                runCatching {
                    val receipt = File(version, "pack.tsv")
                    require(receipt.length() in 1..16_384)
                    val pack = TexturePackCatalogue.parse(receipt.readText()).single()
                    pack.takeIf { directory(context, it).canonicalPath == version.canonicalPath && installed(context, it) }
                }.getOrNull()
            }
        }
        return (remote + local).distinctBy { it.id to it.sha256 }.sortedWith(compareBy({ it.title.lowercase() }, { it.version }))
    }

    private fun connection(url: String): HttpsURLConnection {
        var current = url
        repeat(6) {
            TexturePackCatalogue.https(current, download = true)
            val connection = URI(current).toURL().openConnection() as HttpsURLConnection
            connection.connectTimeout = 15_000; connection.readTimeout = 20_000
            connection.instanceFollowRedirects = false; connection.useCaches = false
            connection.setRequestProperty("Accept", "application/octet-stream")
            try {
                val code = connection.responseCode
                if (code in setOf(301, 302, 303, 307, 308)) {
                    current = URI(current).resolve(connection.getHeaderField("Location") ?: error("Missing redirect URL")).toString()
                    connection.disconnect()
                } else {
                    check(code == 200) { "GitHub download unavailable ($code)" }
                    return connection
                }
            } catch (failure: Throwable) { connection.disconnect(); throw failure }
        }
        error("Too many download redirects")
    }

    suspend fun refresh(context: Context): List<TexturePack> = catalogueLock.withLock {
        withContext(Dispatchers.IO) {
            val coroutine = currentCoroutineContext()
            val conn = connection(CATALOGUE_URL)
            val bytes = try {
                require(conn.contentLengthLong <= 1024 * 1024)
                conn.inputStream.use { Ps1CheatUpdatePackage.readBounded(it, 1024 * 1024) { coroutine.ensureActive() } }
            } finally { conn.disconnect() }
            TexturePackCatalogue.parse(utf8(bytes))
            coroutine.ensureActive()
            val atomic = catalogue(context)
            val output = atomic.startWrite()
            try { output.write(bytes); atomic.finishWrite(output) }
            catch (failure: Throwable) { atomic.failWrite(output); throw failure }
            check(atomic.openRead().use { Ps1CheatUpdatePackage.readBounded(it, 1024 * 1024) }.contentEquals(bytes))
            load(context)
        }
    }

    suspend fun install(context: Context, pack: TexturePack, progress: suspend (Long, Long) -> Unit): File = installLock.withLock {
        // Validate even locally obtained metadata before using an ID/hash as filesystem components.
        require(TexturePackCatalogue.parse(TexturePackCatalogue.HEADER + "\n" + pack.row()).single() == pack)
        withContext(Dispatchers.IO) {
            val coroutine = currentCoroutineContext()
            val parent = File(root(context), "packs").apply { check(isDirectory || mkdirs()) }
            val target = directory(context, pack)
            if (installed(context, pack)) return@withContext target
            check(parent.usableSpace > pack.downloadBytes + pack.expandedBytes + 64L * 1024 * 1024) { "Not enough storage for this pack" }
            // Do not delete or overwrite a prior directory, even if it looks damaged.
            check(!target.exists()) { "Existing pack folder needs manual recovery" }
            val stage = Files.createTempDirectory(parent.toPath(), ".download-").toFile()
            try {
                val archive = File(stage, "pack.zip")
                val conn = connection(pack.downloadUrl)
                try {
                    require(conn.contentLengthLong == -1L || conn.contentLengthLong == pack.downloadBytes)
                    conn.inputStream.use { input -> archive.outputStream().buffered().use { out ->
                        val buffer = ByteArray(64 * 1024)
                        var done = 0L
                        var lastProgress = 0L
                        val start = System.nanoTime()
                        while (true) {
                            coroutine.ensureActive()
                            check(System.nanoTime() - start < 3_600_000_000_000L) { "Texture download timed out" }
                            val count = input.read(buffer)
                            if (count < 0) break
                            done += count
                            require(done <= pack.downloadBytes)
                            out.write(buffer, 0, count)
                            if (System.nanoTime() - lastProgress >= 250_000_000L || done == pack.downloadBytes) {
                                progress(done, pack.downloadBytes); lastProgress = System.nanoTime()
                            }
                        }
                        require(done == pack.downloadBytes)
                    } }
                } finally { conn.disconnect() }
                val data = File(stage, "data").apply { check(mkdir()) }
                TexturePackArchive.extract(archive, data, pack) { coroutine.ensureActive() }
                File(data, "pack.tsv").writeText(TexturePackCatalogue.HEADER + "\n" + pack.row() + "\n")
                coroutine.ensureActive()
                check(target.parentFile!!.isDirectory || target.parentFile!!.mkdirs())
                check(!target.exists() && data.renameTo(target)) { "Unable to finish installing the texture pack" }
                target
            } finally {
                // Only this operation's randomly created staging directory; installed/user packs stay intact.
                stage.deleteRecursively()
            }
        }
    }
}
