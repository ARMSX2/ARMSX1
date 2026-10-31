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
 * The one place a user-driven save-state load goes through.
 *
 * ## What it is guarding against
 *
 * A save state and a memory card are two timelines of the same playthrough, and the player can
 * move one without the other. Save a state, keep playing, save to the card in-game, then load
 * that old state: the console is now *behind* its own card. Real PS1 games notice — the save
 * file they are told to expect does not match the directory they cached, and depending on the
 * title they cope, refuse to load their own save, or misbehave in ways that look like a corrupt
 * card. The player can lose hours without ever being told why.
 *
 * In this core it is worse than an inconsistency. Loading a state restores the card image along
 * with the machine, and the card is flushed back to `slot1.mcd` / `slot2.mcd` at shutdown, so the
 * in-game save made after the state is *silently discarded*. That is the sentence the dialog has
 * to get across, and it is why the warning is worth interrupting for.
 *
 * ## How the answer is arrived at
 *
 * The core does the detecting, in [NativeApp.loadStateFromSlotChecked]. Every state records a
 * fingerprint of both cards (content hash, plus a write counter and a per-attach nonce so the
 * *direction* is knowable and not just the difference), and the load compares it against the
 * cards as they are now — during its identity phase, before a single byte of the machine has been
 * written. So a refusal is free: cancelling leaves the session exactly as it was, and "load
 * anyway" is a second call with the check waived rather than an unwind.
 *
 * Three outcomes reach here:
 *  - [NativeApp.STATE_ERR_CARD_NEWER] — the card carries writes the state does not. The
 *    dangerous direction, and the one worth a firm warning.
 *  - [NativeApp.STATE_ERR_CARD_DIVERGED] — different, but nothing establishes which way (a card
 *    imported, erased, or written by another session). Softer wording; same choice.
 *  - anything else — either a clean load or a genuine failure, both passed straight back.
 *
 * A state written before fingerprints existed has none, and is loaded in silence. That is
 * deliberate: "no fingerprint" is not evidence of anything, and a warning the user cannot act on
 * teaches them to dismiss the dialog without reading it, which costs more than it saves.
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
