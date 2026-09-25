package com.armsx2

import java.util.concurrent.atomic.AtomicBoolean

class DiagnosticExportGate {
    enum class Result { SUCCESS, FAILED, BUSY }

    private val active = AtomicBoolean()

    fun run(write: () -> Boolean): Result {
        if (!active.compareAndSet(false, true)) return Result.BUSY
        return try {
            if (write()) Result.SUCCESS else Result.FAILED
        } catch (_: Exception) {
            Result.FAILED
        } finally {
            active.set(false)
        }
    }
}
