package com.armsx2.ui.saves

import com.armsx2.i18n.I18n
import com.armsx2.runtime.MainActivityRuntime
import com.armsx2.ui.common.GlobalConfirm
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.suspendCancellableCoroutine
import kotlinx.coroutines.withContext
import kr.co.iefriends.pcsx2.NativeApp
import kotlin.coroutines.resume

/**
 * Shared load result and confirmation flow. PSX Lab states restore the card protocol,
 * but deliberately exclude card contents, so its checked adapter preserves current
 * in-game saves and reports success/failure directly. Retain advisory handling for
 * any future format that embeds card contents and can establish real divergence.
 */
object SaveStateGuard {

    /** Off by default. On, every load skips the check — the user has told us they know. */
    const val PREF_SKIP_CARD_WARNING = "savestate.skipCardWarning"

    /** Stand-in when the JNI call itself threw. Must not collide with a real PSX_STATE_ERR_*
     *  code, and in particular must never look like one of the two advisory card verdicts —
     *  a failed call is a failure, not a question to put to the user. */
    private const val JNI_THREW = Int.MIN_VALUE

    enum class Outcome {
        /** The state was applied. */
        Loaded,

        /** The user was warned and chose not to load. Nothing was touched. */
        Cancelled,

        /** A real failure — empty slot, wrong disc, incompatible build, no VM. */
        Failed,
    }

    fun skipWarning(): Boolean =
        runCatching { MainActivityRuntime.prefs.getBoolean(PREF_SKIP_CARD_WARNING, false) }
            .getOrDefault(false)

    /**
     * Load [slot], asking first if the memory cards have moved on since the state was taken.
     *
     * Suspends across the dialog, so a caller can simply act on the [Outcome]. Safe to call from
     * any dispatcher: the JNI call (which blocks up to two seconds waiting for the emulation
     * thread) is moved to [Dispatchers.IO], and the prompt is raised on Main.
     */
    suspend fun load(slot: Int): Outcome {
        val skip = skipWarning()

        val first = withContext(Dispatchers.IO) {
            runCatching { NativeApp.loadStateFromSlotChecked(slot, skip) }
                .getOrDefault(JNI_THREW)
        }

        if (first == NativeApp.STATE_OK) return Outcome.Loaded

        val diverged =
            first == NativeApp.STATE_ERR_CARD_NEWER || first == NativeApp.STATE_ERR_CARD_DIVERGED

        if (!diverged) return Outcome.Failed

        val cardIsNewer = first == NativeApp.STATE_ERR_CARD_NEWER

        val confirmed = withContext(Dispatchers.Main) {
            suspendCancellableCoroutine { cont ->
                GlobalConfirm.ask(
                    title = I18n.get(
                        if (cardIsNewer) "savestate.cardWarning.newer.title"
                        else "savestate.cardWarning.diverged.title",
                    ),
                    message = I18n.get(
                        if (cardIsNewer) "savestate.cardWarning.newer.body"
                        else "savestate.cardWarning.diverged.body",
                    ),
                    confirmLabel = I18n.get("savestate.cardWarning.loadAnyway"),
                    // Loading is what discards the card save, so the affirmative button is the
                    // destructive one. Cancel stays pre-focused (ConfirmOverlay puts the safe
                    // choice first) — a blind A press must not be the one that loses data.
                    destructive = true,
                    onCancel = { if (cont.isActive) cont.resume(false) },
                    onConfirm = { if (cont.isActive) cont.resume(true) },
                )
            }
        }

        if (!confirmed) return Outcome.Cancelled

        val second = withContext(Dispatchers.IO) {
            runCatching { NativeApp.loadStateFromSlotChecked(slot, true) }
                .getOrDefault(JNI_THREW)
        }

        return if (second == NativeApp.STATE_OK) Outcome.Loaded else Outcome.Failed
    }
}
