package com.armsx2.ui.common

import android.content.Context
import android.content.SharedPreferences
import android.net.Uri
import coil.imageLoader
import coil.memory.MemoryCache
import coil.request.ImageRequest
import coil.request.SuccessResult
import coil.size.Precision
import java.io.File
import java.io.FileNotFoundException
import java.util.concurrent.ConcurrentHashMap

/** Keep successful fallback choices across recycled tiles and app restarts. */
internal object CoverArtCache {
    private var preferences: SharedPreferences? = null
    private var resetPending = false
    private val missing = ConcurrentHashMap.newKeySet<String>()
    private val prefetching = ConcurrentHashMap.newKeySet<String>()

    @Synchronized private fun preferences(context: Context): SharedPreferences {
        val prefs = preferences ?: context.applicationContext
            .getSharedPreferences("cover-art-sources", Context.MODE_PRIVATE).also { preferences = it }
        if (resetPending) {
            prefs.edit().clear().apply()
            resetPending = false
        }
        return prefs
    }

    @Synchronized fun refresh() {
        missing.clear()
        if (preferences == null) resetPending = true else preferences!!.edit().clear().apply()
    }

    private fun identity(model: Any): String {
        val file = when {
            model is File -> model
            model is String && model.startsWith("file:") -> Uri.parse(model).path?.let(::File)
            else -> null
        }
        return if (file == null) model.toString()
        else "${file.absolutePath}:${file.lastModified()}:${file.length()}"
    }

    fun memoryKey(model: Any, size: Int = 256): MemoryCache.Key =
        MemoryCache.Key("armsx-cover:$size:${identity(model)}")

    fun ordered(context: Context, models: List<Any>): List<Any> {
        val sources = models.map(::identity)
        val preferred = preferences(context).getString(CoverSourcePolicy.key(sources), null)
        return CoverSourcePolicy.order(sources, preferred).map { models[it] }
            .filterNot { identity(it) in missing }
    }

    fun succeeded(context: Context, models: List<Any>, model: Any) {
        val sources = models.map(::identity)
        val winner = identity(model)
        val prefs = preferences(context)
        val key = CoverSourcePolicy.key(sources)
        if (prefs.getString(key, null) != winner) prefs.edit().putString(key, winner).apply()
    }

    fun failed(model: Any?, cause: Throwable) {
        if (model != null && (cause is FileNotFoundException ||
                cause is coil.network.HttpException && cause.response.code in setOf(404, 410))) {
            missing.add(identity(model))
        }
    }

    fun request(context: Context, model: Any?, crossfade: Boolean = false, size: Int = 256): ImageRequest =
        ImageRequest.Builder(context).data(model).size(size, size)
            .memoryCacheKey(model?.let { memoryKey(it, size).key })
            .precision(Precision.INEXACT).allowHardware(true).crossfade(crossfade).build()

    suspend fun prefetch(context: Context, models: List<Any>): Boolean {
        if (models.isEmpty()) return false
        val key = CoverSourcePolicy.key(models.map(::identity))
        if (!prefetching.add(key)) return false
        try {
            val loader = context.imageLoader
            for (model in ordered(context, models)) {
                if (loader.memoryCache?.get(memoryKey(model)) != null) return true
                when (val result = loader.execute(request(context, model))) {
                    is SuccessResult -> { succeeded(context, models, model); return true }
                    is coil.request.ErrorResult -> failed(model, result.throwable)
                }
            }
            return false
        } finally {
            prefetching.remove(key)
        }
    }
}
