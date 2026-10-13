package com.armsx2.cheats

import android.content.Context
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.currentCoroutineContext
import kotlinx.coroutines.ensureActive
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import kotlinx.coroutines.withContext
import java.net.URL
import javax.net.ssl.HttpsURLConnection

object Ps1CheatDownloader {
    // Public data only. No account, token or APK download required.
    const val UPDATE_URL = "https://raw.githubusercontent.com/ARMSX2/ARMSX1/android-compose/cheats/ps1-cheats-update.zip"
    private val downloadLock = Mutex()

    suspend fun download(context: Context): Boolean = downloadLock.withLock {
        withContext(Dispatchers.IO) {
            val coroutine = currentCoroutineContext()
            val connection = URL(UPDATE_URL).openConnection() as HttpsURLConnection
            connection.connectTimeout = 15_000
            connection.readTimeout = 20_000
            connection.instanceFollowRedirects = false
            connection.useCaches = false
            connection.setRequestProperty("Accept", "application/octet-stream")
            try {
                coroutine.ensureActive()
                check(connection.responseCode == 200) { "GitHub cheat download unavailable (${connection.responseCode})" }
                require(connection.contentLengthLong <= Ps1CheatUpdatePackage.MAX_DOWNLOAD)
                val started = System.nanoTime()
                val bytes = connection.inputStream.use {
                    Ps1CheatUpdatePackage.readBounded(it, Ps1CheatUpdatePackage.MAX_DOWNLOAD) {
                        coroutine.ensureActive()
                        check(System.nanoTime() - started < 120_000_000_000L) { "Cheat download timed out" }
                    }
                }
                coroutine.ensureActive()
                Ps1CheatDatabase.installUpdate(context, bytes)
            } finally {
                connection.disconnect()
            }
        }
    }
}
