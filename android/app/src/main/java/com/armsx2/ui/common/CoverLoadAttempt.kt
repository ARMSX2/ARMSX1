package com.armsx2.ui.common

/** Immutable request state: stale callbacks cannot skip a model or restart an exhausted pass. */
internal data class CoverLoadAttempt(
    val index: Int = 0, val retry: Int = 0, val transientFailure: Boolean = false, val waiting: Boolean = false,
) {
    fun failed(count: Int, transient: Boolean): CoverLoadAttempt {
        if (waiting || count <= 0) return this
        val retryable = transientFailure || transient
        return when {
            index < count - 1 -> copy(index = index + 1, transientFailure = retryable)
            retryable && retry < 2 -> copy(transientFailure = true, waiting = true)
            else -> this
        }
    }
    fun resume(): CoverLoadAttempt = if (waiting && retry < 2) CoverLoadAttempt(retry = retry + 1) else this
    val delayMillis: Long get() = 2_000L * (retry + 1)
}
