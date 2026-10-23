package com.armsx2.core

import java.io.File
import java.io.FileOutputStream
import java.io.IOException
import java.nio.file.AtomicMoveNotSupportedException
import java.nio.file.Files
import java.nio.file.StandardCopyOption
import java.security.MessageDigest
import org.json.JSONObject

/** Records paths actually mounted by SwanStation, rather than guessing from cover/title text. */
internal object Ps1CardBindings {
    private fun index(root: File, source: String, imported: Boolean = false): File {
        require(source.isNotBlank())
        val key = MessageDigest.getInstance("SHA-256").digest(source.toByteArray(Charsets.UTF_8))
            .joinToString("") { "%02x".format(it.toInt() and 255) }
        return File(root, (if (imported) ".import-game-" else ".game-") + "$key.cards")
    }

    fun safeCard(root: File, name: String): File? {
        if (name.isBlank()) return null
        val file = File(name).let { if (it.isAbsolute) it else File(root, name) }
        return file.takeIf {
            it.extension.equals("mcd", true) &&
                it.canonicalFile.parentFile == root.canonicalFile
        }
    }

    fun remember(root: File, source: String, cards: List<File>) {
        require(cards.size == 2)
        val names = cards.map { safeCard(root, it.absolutePath)?.name
            ?: throw IOException("The mounted memory card is outside the active data folder.") }
        root.mkdirs()
        val target = index(root, source)
        val temporary = File(target.parentFile, target.name + ".tmp")
        try {
            val json = JSONObject().put("source", source)
                .put("slot1", names[0]).put("slot2", names[1])
            FileOutputStream(temporary).use {
                it.write(json.toString().toByteArray(Charsets.UTF_8))
                it.fd.sync()
            }
            try {
                Files.move(temporary.toPath(), target.toPath(),
                    StandardCopyOption.ATOMIC_MOVE, StandardCopyOption.REPLACE_EXISTING)
            } catch (_: AtomicMoveNotSupportedException) {
                Files.move(temporary.toPath(), target.toPath(), StandardCopyOption.REPLACE_EXISTING)
            }
        } finally {
            temporary.delete()
        }
    }

    fun resolve(root: File, source: String, slot: Int): File? {
        require(slot in 1..2)
        return runCatching {
            val json = JSONObject(index(root, source).readText())
            if (json.optString("source") != source) null
            else safeCard(root, json.optString("slot$slot"))
        }.getOrNull()
    }

    fun imported(root: File, source: String, slot: Int): File? = runCatching {
        val json = JSONObject(index(root, source, true).readText())
        if (json.optString("source") != source) null
        else safeCard(root, json.optString("slot$slot"))?.takeIf { it.isFile }
    }.getOrNull()

    /** Persist only explicit imported slots, without changing the other port. */
    fun rememberImported(root: File, source: String, slots: Map<Int, File>) {
        val target = index(root, source, true)
        val json = runCatching { JSONObject(target.readText()) }.getOrDefault(JSONObject())
        json.put("source", source)
        slots.forEach { (slot, file) ->
            require(slot in 1..2)
            json.put("slot$slot", safeCard(root, file.absolutePath)?.name
                ?: throw IOException("Imported card is outside the memory-card folder."))
        }
        root.mkdirs()
        val temporary = File.createTempFile(".card-binding-", ".tmp", root)
        try {
            FileOutputStream(temporary).use { it.write(json.toString().toByteArray()); it.fd.sync() }
            try { Files.move(temporary.toPath(), target.toPath(), StandardCopyOption.ATOMIC_MOVE, StandardCopyOption.REPLACE_EXISTING) }
            catch (_: AtomicMoveNotSupportedException) { Files.move(temporary.toPath(), target.toPath(), StandardCopyOption.REPLACE_EXISTING) }
        } finally { temporary.delete() }
    }
}
