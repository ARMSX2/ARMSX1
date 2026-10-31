package com.armsx2

import android.app.Activity
import android.content.ComponentName
import android.content.Context
import android.content.Intent
import android.content.ServiceConnection
import android.os.Bundle
import android.os.Handler
import android.os.IBinder
import android.os.Looper
import android.os.Message
import android.os.Messenger
import android.util.Log
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.snapshotFlow
import com.armsx2.runtime.MainActivityRuntime
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import com.armsx2.discord.DiscordIpc

/**
 * Discord rich presence, and which friends are in ARMSX2 right now.
 *
 * Discord carries the entire social graph — the friendships, the online status, and the "is this
 * person in the same game" grouping are all theirs. That is the whole point of doing it this way:
 * no account system, no friend database, no server, and nothing of the user's kept anywhere by us
 * except an OAuth token in this app's own prefs.
 *
 * Foreground only, by construction. The SDK stops when the app does, so friend changes are seen
 * while ARMSX2 is open and not otherwise. Background delivery would mean push infrastructure, which
 * is exactly the cost this design exists to avoid.
 *
 * Opt-in and off by default: it is an account link, so nothing happens until the user asks.
 */
/**
 * A Discord friend who has ARMSX2 open.
 *
 * [game] and [serial] come from the rich presence they are publishing — which this same object
 * wrote on their device — so "what are they playing" needs no lookup service. A friend sitting in
 * the library publishes neither, which is what [inLibrary] keys off.
 */
data class DiscordFriend(
    val name: String,
    val game: String,
    val serial: String,
    val avatarUrl: String,
) {
    /** In ARMSX2 but not in a game. Distinct from being in a game we could not name. */
    val inLibrary: Boolean get() = game.isBlank() && serial.isBlank()

    /**
     * Cover art for what they are playing, resolved the same way the library resolves its own.
     *
     * Derived from the serial here rather than read out of their presence assets: Discord may hand
     * back a proxied form of an image URL, and re-deriving keeps a friend's row looking exactly like
     * the same game does on the home screen — including honouring the 2D/3D preference, which is
     * this device's choice to make, not theirs.
     */
    // ARMSX1: PS1 box art comes from xlenore/psx-covers (never ps2-covers).
    val coverUrl: String? get() = serial.takeIf { it.isNotBlank() }?.let { s ->
        if (CoverArtStyle.use3d.value)
            "https://raw.githubusercontent.com/xlenore/psx-covers/main/covers/3d/$s.png"
        else
            "https://raw.githubusercontent.com/xlenore/psx-covers/main/covers/default/$s.jpg"
    }
}

object DiscordPresence {
    private const val TAG = "DiscordPresence"
    private const val PREF_ENABLED = "discord.enabled"
    private const val PREF_TOKEN = "discord.token"
    private const val PREF_NOTIFY = "discord.notifyInGame"

    /** Seconds between RA rich-presence re-checks. Discord rate-limits presence updates, so this
     *  stays well clear of it while still following the game within a few seconds. */
    private const val RA_REPUSH_SECONDS = 15

    /** Discord truncates a long state line; stay under it rather than being cut mid-word. */
    private const val RA_STATE_MAX = 126

    // ---- stuck-forever guards ------------------------------------------------------------------
    //
    // AUTHORIZING and CONNECTING are both driven entirely by SDK callbacks, and a callback that
    // never arrives leaves them set for the life of the process. The Friends panel then renders a
    // spinner with no way out — the reported "stuck on trying to connect forever". Every one of
    // these has a real cause the user can act on (they closed the browser tab, the redirect URI is
    // not registered, there is no network, the helper process did not come up), so each dwell gets
    // a deadline and a message rather than an indefinite spinner.

    /** Browser sign-in. Generous: this covers a real person logging in and pressing Authorize. */
    private const val AUTHORIZE_TIMEOUT_MS = 120_000L

    /** Handshake with a token we already have. No human in the loop, so much tighter. */
    private const val CONNECT_TIMEOUT_MS = 45_000L

    /** The :discord helper answering its first MSG_QUERY. A cold process bind, nothing more. */
    private const val HELPER_TIMEOUT_MS = 20_000L

    // Mirrors BridgeStatus in cpp/discord_bridge.cpp.
    const val DISABLED = 0
    const val DISCONNECTED = 1
    const val AUTHORIZING = 2
    const val CONNECTING = 3
    const val CONNECTED = 4
    const val FAILED = 5

    val status = mutableStateOf(DISABLED)
    val friends = mutableStateOf<List<DiscordFriend>>(emptyList())
    val error = mutableStateOf<String?>(null)

    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Main)
    private var pollJob: Job? = null
    private var presenceJob: Job? = null
    private var started = false

    // ---- helper-process link -------------------------------------------------------------
    //
    // The Discord SDK runs in :discord, not here. That is a licensing boundary: ARMSX2 is GPL-3.0+
    // and the SDK is proprietary, so they are two programs exchanging messages rather than one
    // linked binary. Nothing in this file may reference a com.discord class or DiscordNative —
    // both would fail to resolve in this process, which is exactly the intended guarantee.

    private var helper: Messenger? = null
    private var bound = false

    /** Last snapshot the helper sent. Read by the poll loop; written on the main thread. */
    private var lastState: Bundle? = null

    private val incoming = Messenger(Handler(Looper.getMainLooper()) { msg ->
        if (msg.what == DiscordIpc.MSG_STATE) lastState = msg.data
        true
    })

    /**
     * A Connect press that arrived before the bind completed, waiting to be replayed.
     *
     * ★ This is the "first press of Connect does nothing" bug. bindService() is asynchronous, so on
     * the very first press [helper] is still null and [send] silently drops the message — and
     * onServiceConnected below only ever replayed MSG_START, never the authorize. The user pressed
     * Connect, nothing happened, and the only recovery was pressing it again.
     */
    private var pendingAuthorize = false

    private val connection = object : ServiceConnection {
        override fun onServiceConnected(name: ComponentName?, service: IBinder?) {
            helper = service?.let { Messenger(it) }
            Log.i(TAG, "helper process connected")
            // Re-issue whatever the helper needs to know: a reconnect means a fresh process that
            // remembers nothing, so anything set before the bind has to be replayed.
            send(DiscordIpc.MSG_START, Bundle().apply { putString(DiscordIpc.DATA_TOKEN, savedToken) })
            // Launched rather than called: pushPresence computes the RA line off the main thread
            // now, and this callback is not a coroutine. MSG_START is already on the wire above and
            // the helper handles both on one queue, so its `started` guard on MSG_SET_PLAYING still
            // passes by the time this lands.
            scope.launch { pushPresence() }
            if (pendingAuthorize) {
                pendingAuthorize = false
                Log.i(TAG, "replaying the queued authorize now the helper is up")
                send(DiscordIpc.MSG_AUTHORIZE)
            }
        }

        override fun onServiceDisconnected(name: ComponentName?) {
            // The helper died — a crash in the SDK, or the system reclaiming it. Nothing here is
            // lost: rebinding replays the token and the presence.
            Log.w(TAG, "helper process disconnected")
            helper = null
            // ★ Drop the snapshot too. Keeping it meant the poll below went on re-applying the last
            // status the dead process reported — forever. A helper that died mid-authorize therefore
            // pinned the UI on AUTHORIZING with nothing left alive to ever change it.
            lastState = null
        }
    }

    /** Consecutive failed binds, and the earliest time the next attempt may run. The poll loop
     *  below calls bindHelper() once a second forever; a bind that keeps failing therefore
     *  retried — and logged — every single second for the life of the process. That is real
     *  battery and a logcat firehose, for a helper that is not coming back on its own. */
    private var bindFailures = 0
    private var nextBindAtMs = 0L

    private fun bindHelper() {
        if (bound) return
        val now = android.os.SystemClock.elapsedRealtime()
        if (now < nextBindAtMs) return

        val ctx = MainActivityRuntime.instance?.applicationContext ?: return
        val intent = Intent(ctx, com.armsx2.discord.DiscordService::class.java)
        bound = runCatching {
            ctx.bindService(intent, connection, Context.BIND_AUTO_CREATE)
        }.getOrDefault(false)

        if (bound) {
            bindFailures = 0
            nextBindAtMs = 0L
            return
        }

        // 1s, 2s, 4s ... capped at 64s. Logged only on the FIRST failure of a run: the repeat
        // carries no information, and burying every other diagnostic under it has actively cost
        // debugging time on this project.
        if (bindFailures == 0) Log.w(TAG, "could not bind the Discord helper; backing off")
        nextBindAtMs = now + (1L shl minOf(bindFailures, 6)) * 1000L
        bindFailures++
    }

    /**
     * Drop the binding, which is what actually ends the helper.
     *
     * The service is bound-only and never started, so the last unbind destroys the process — and
     * with it every trace of the Discord SDK. That is the property worth preserving: with the
     * feature off, the proprietary library is not merely idle, it is not loaded at all.
     */
    private fun unbindHelper() {
        val ctx = MainActivityRuntime.instance?.applicationContext
        if (bound && ctx != null) runCatching { ctx.unbindService(connection) }
        bound = false
        helper = null
        lastState = null
    }

    private fun send(what: Int, data: Bundle? = null) {
        val target = helper ?: return
        runCatching {
            target.send(Message.obtain(null, what).apply {
                if (data != null) this.data = data
                replyTo = incoming
            })
        }.onFailure { Log.w(TAG, "ipc send($what) failed: ${it.message}") }
    }

    // Last RA line published, so the poll below only re-pushes when it actually changes.
    // Main thread only: written by [sendPresence], read by the poll loop.
    private var lastRaPresence = ""

    /**
     * The RetroAchievements line for Discord: the game's own rich presence, plus how many
     * achievements are unlocked and whether this is a Hardcore or Softcore run.
     *
     *     Exploring Ivalice, Lv 34 · 12/45 · Hardcore
     *
     * Empty when RA is off or the game has no set, in which case the bridge falls back to the
     * serial so the in-a-game marker still holds.
     *
     * The count follows the MODE: in hardcore it reports hardcore unlocks, otherwise any unlock.
     * Reporting softcore progress next to a "Hardcore" label would be actively misleading, which
     * is the whole reason the label is there.
     *
     * ★ Suspending, and computed OFF the main thread. getAchievementsJSON() hands back the whole
     * achievement set as one string and parseAchievementItems() walks all of it, so on a large set
     * this is a real JSON parse — and it runs DURING GAMEPLAY, every RA_REPUSH_SECONDS, for as long
     * as Discord is connected and RA is on. On the UI thread that was a visible frame hitch every
     * 15 seconds, for a line nobody on this device ever reads.
     *
     * The natives are safe off it. All three take the same recursive lock the emulation thread's
     * frame update holds, so they are already serialised against it (frontend/achievements.cpp:
     * armsx_ach_get_json, armsx_ach_get_rich_presence; armsx_ach_hardcore_active is a bare
     * `return false`), and the prologue every RA JNI entry point runs — EnsureAchievementsReady() —
     * is already reached off the UI thread by the library's per-game hash lookup on its own sync
     * thread. Dispatchers.IO rather than Default because what this waits on is that lock, held for
     * the length of a frame update, not CPU of its own.
     *
     * [game] is passed in rather than read here: it is Compose snapshot state the main thread owns,
     * and the line should describe the game the caller actually saw.
     */
    private suspend fun raPresence(game: GameInfo?): String {
        if (game == null) return ""
        return withContext(Dispatchers.IO) { raPresenceLine() }
    }

    /** [raPresence]'s body: three JNI calls and a JSON parse. Never call this on the main thread. */
    private fun raPresenceLine(): String {
        val presence = runCatching { kr.co.iefriends.pcsx2.NativeApp.getRichPresence().orEmpty() }
            .getOrDefault("").trim()

        val badge = runCatching {
            val items = com.armsx2.ui.achievements.parseAchievementItems(
                kr.co.iefriends.pcsx2.NativeApp.getAchievementsJSON().orEmpty(),
            )
            if (items.isEmpty()) return@runCatching ""
            val hardcore = kr.co.iefriends.pcsx2.NativeApp.isHardcoreMode()
            val unlocked = if (hardcore) {
                // Guard the unknown case: -1 has every bit set, so a bare mask test would report
                // an unearned hardcore unlock on any build that omits the field.
                items.count { it.unlockedMask > 0 && (it.unlockedMask and 2) != 0 }
            } else {
                // Mask when present, boolean otherwise; a hardcore unlock counts as softcore too.
                items.count { if (it.unlockedMask >= 0) it.unlockedMask != 0 else it.unlocked }
            }
            // Emoji rather than plain words: this is one short line on someone else's screen, and
            // a trophy plus a colour reads at a glance where "12/45 Hardcore" needs parsing.
            // "Casual" not "Softcore" -- that is what our own hardcore toggle calls it, and the
            // two names for one mode in one app would be worse than either name alone.
            "\uD83C\uDFC6 $unlocked/${items.size}  ·  " +
                if (hardcore) "\uD83D\uDD34 Hardcore" else "\uD83D\uDD35 Casual"
        }.getOrDefault("")

        val line = listOf(presence, badge).filter { it.isNotBlank() }.joinToString("  ·  ")
        // Discord truncates a long state; trim the free-text presence rather than the badge, since
        // the counts are the part that cannot be inferred from anywhere else.
        return if (line.length <= RA_STATE_MAX) line
        else if (badge.isNotBlank()) badge
        else line.take(RA_STATE_MAX)
    }

    /**
     * Cover art Discord can actually fetch.
     *
     * NOT [GameInfo.coverUrl]: that prefers a cover file sitting next to the disc image and returns
     * it as `file:///storage/...`, which is meaningful only on this device — Discord's servers
     * fetch the large image themselves, so a local path publishes as a broken image. The library
     * keeps its local art; presence falls back to the psx-covers URL for the same serial, and to
     * empty (which makes the bridge use the ARMSX logo) when there is no serial to derive one from.
     */
    private fun remoteCoverUrl(game: GameInfo?): String {
        val direct = game?.coverUrl
        if (direct != null && direct.startsWith("http")) return direct
        val s = game?.serial?.takeIf { it.isNotBlank() } ?: return ""
        return if (CoverArtStyle.use3d.value)
            "https://raw.githubusercontent.com/xlenore/psx-covers/main/covers/3d/$s.png"
        else
            "https://raw.githubusercontent.com/xlenore/psx-covers/main/covers/default/$s.jpg"
    }

    /**
     * Re-publish the current game; used on (re)connect and whenever the running game changes.
     *
     * Suspends only for [raPresence]'s off-thread work — [withContext] returns to the caller's
     * context, so the assignment and the IPC in [sendPresence] land back on the main thread.
     */
    private suspend fun pushPresence() {
        val game = MainActivityRuntime.currentGame.value
        sendPresence(game, raPresence(game))
    }

    /**
     * Publish [ra] for [game]. MAIN THREAD ONLY: it writes [lastRaPresence], reads the cover-art
     * preference, and talks to the helper.
     *
     * Split out of [pushPresence] so the poll loop — which has already computed the line in order to
     * compare it against [lastRaPresence] — does not compute it a second time just to send it. That
     * was two full achievement parses per re-push, which is exactly the work this moved off the UI
     * thread in the first place.
     */
    private fun sendPresence(game: GameInfo?, ra: String) {
        lastRaPresence = ra
        send(DiscordIpc.MSG_SET_PLAYING, Bundle().apply {
            putString(DiscordIpc.DATA_SERIAL, game?.serial.orEmpty())
            putString(DiscordIpc.DATA_TITLE, game?.let { it.displayTitle(false) }.orEmpty())
            putString(DiscordIpc.DATA_COVER, remoteCoverUrl(game))
            putString(DiscordIpc.DATA_RA, ra)
        })
    }

    /**
     * False when the SDK was not staged at build time — the UI hides the whole section.
     *
     * Answered by the helper process, so it is unknown until the first snapshot lands. Defaults to
     * true meanwhile: hiding the feature and then revealing it a second later looks broken, while
     * the reverse is invisible on a build that has the SDK.
     */
    private var helperAvailable = true

    fun available(): Boolean = helperAvailable

    var enabled: Boolean
        get() = available() && runCatching {
            MainActivityRuntime.prefs.getBoolean(PREF_ENABLED, false)
        }.getOrDefault(false)
        set(value) {
            runCatching { MainActivityRuntime.prefs.edit().putBoolean(PREF_ENABLED, value).apply() }
            if (value) start() else signOut()
        }

    private var savedToken: String
        get() = runCatching { MainActivityRuntime.prefs.getString(PREF_TOKEN, "") ?: "" }.getOrDefault("")
        set(value) {
            runCatching { MainActivityRuntime.prefs.edit().putString(PREF_TOKEN, value).apply() }
        }

    /** Announce on the in-game OSD when a friend joins. Default on: it is the point of the feature. */
    var notifyInGame: Boolean
        get() = runCatching { MainActivityRuntime.prefs.getBoolean(PREF_NOTIFY, true) }.getOrDefault(true)
        set(value) {
            runCatching { MainActivityRuntime.prefs.edit().putBoolean(PREF_NOTIFY, value).apply() }
            notifyState.value = value
        }

    /** Mirror of [notifyInGame] for Compose, which cannot observe SharedPreferences. */
    val notifyState = mutableStateOf(true)

    // Who we had last poll, so a friend appearing can be told apart from a friend still being here.
    private var knownFriends: Set<String> = emptySet()

    // Whether the first poll after connecting has been absorbed.
    //
    // An explicit flag, NOT "is the previous set empty". Inferring it was a real bug: connect with
    // nobody online and the very next arrival looks identical to the seed poll — empty before,
    // non-empty now — so the one notification most worth showing was the one always swallowed.
    private var seededFriends = false

    /** Friend who just came online, for the library banner to show. Cleared once it has been seen. */
    val justOnline = mutableStateOf<DiscordFriend?>(null)

    /** The signed-in Discord account: name and avatar. Null until connected. */
    val self = mutableStateOf<DiscordFriend?>(null)

    private const val FIELD_SEP = DiscordIpc.FIELD_SEP
    private const val RECORD_SEP = DiscordIpc.RECORD_SEP

    /**
     * Kept for call-site compatibility; there is nothing to bind any more.
     *
     * The SDK used to need ARMSX2's Activity to launch sign-in. It now lives in :discord and uses
     * DiscordAuthActivity over there, so handing it an Activity from this process would be handing
     * it a reference from the wrong process. Deliberately a no-op rather than deleted: the callers
     * are lifecycle hooks, and a missing one would be silent.
     */
    fun attachActivity(activity: Activity) {
        // Binding early means the helper is warm before anyone opens the Friends screen.
        if (enabled) bindHelper()
    }

    /** Bring the client up, reusing a stored token when there is one so sign-in is once, not daily. */
    fun start() {
        if (!available() || !enabled || started) return
        started = true
        notifyState.value = notifyInGame
        bindHelper()
        // Send it here TOO, not only from onServiceConnected.
        //
        // onServiceConnected fires once per binding. After a Disconnect the binding is still up, so
        // signing back in never produced a second callback — the helper kept the torn-down client
        // from MSG_STOP, Connect sent an authorize into it, and nothing happened until the app was
        // restarted and the bind was fresh. MSG_START is idempotent in the bridge (it only creates
        // a client when there isn't one), so sending it on both paths is free.
        //
        // A no-op when the bind is still in flight: send() drops it, and onServiceConnected covers
        // that case a moment later.
        send(DiscordIpc.MSG_START, Bundle().apply { putString(DiscordIpc.DATA_TOKEN, savedToken) })
        startPolling()
        startPresenceWatch()
    }

    fun authorize() {
        if (!available()) return
        // Authorizing implies enabling: someone pressing Connect has opted in, and requiring the
        // toggle first would just be a second thing to press.
        if (!enabled) {
            runCatching { MainActivityRuntime.prefs.edit().putBoolean(PREF_ENABLED, true).apply() }
        }
        if (!started) start()
        bindHelper()
        error.value = null
        gaveUpMessage = null
        connectRetried = false
        nonTerminalSinceMs = android.os.SystemClock.elapsedRealtime()
        Log.i(TAG, "authorize() requested (started=$started, enabled=$enabled)")
        if (helper == null) {
            // The bind is still in flight, so send() would drop this on the floor and nothing would
            // ever replay it. Queue instead — onServiceConnected fires it the moment it can.
            pendingAuthorize = true
            Log.i(TAG, "authorize queued: helper not bound yet")
        } else {
            send(DiscordIpc.MSG_AUTHORIZE)
        }
    }

    fun signOut() {
        pollJob?.cancel(); pollJob = null
        presenceJob?.cancel(); presenceJob = null
        started = false
        savedToken = ""
        knownFriends = emptySet()
        seededFriends = false
        pendingAuthorize = false
        gaveUpMessage = null
        connectRetried = false
        nonTerminalSinceMs = 0L
        justOnline.value = null
        self.value = null
        send(DiscordIpc.MSG_STOP)
        // Then let go of the helper entirely, so the next Connect starts from a clean process
        // rather than reusing one whose client has just been destroyed.
        unbindHelper()
        status.value = if (available()) DISCONNECTED else DISABLED
        friends.value = emptyList()
        error.value = null
    }

    /**
     * Stop waiting, on purpose. Backs the Cancel button on the connecting state.
     *
     * Not [signOut]: this keeps the stored token, so a cancel is a cancel and not a sign-out. It
     * exists because there was previously NO way out of the spinner short of killing the app.
     */
    fun cancelConnect() {
        giveUp(com.armsx2.i18n.I18n.get("friends.cancelled"))
    }

    // ---- stuck-forever guard state -------------------------------------------------------------

    /** When the current AUTHORIZING/CONNECTING/no-snapshot dwell began; 0 when settled. */
    private var nonTerminalSinceMs = 0L

    /**
     * Set once a dwell has blown its deadline. Non-null pins the UI to [FAILED] with this text,
     * regardless of what the helper keeps reporting, until the next [authorize] or [signOut].
     */
    private var gaveUpMessage: String? = null

    /**
     * Land on a terminal state with something the user can act on.
     *
     * Also tears the helper's client down (MSG_STOP) and drops [started], because the SDK refuses a
     * second Authorize() while one it still believes is in flight has not resolved — without this
     * the next Connect press would be silently ignored, which is how the original bug compounded.
     */
    private fun giveUp(message: String) {
        Log.w(TAG, "giving up on the Discord connection: $message")
        gaveUpMessage = message
        nonTerminalSinceMs = 0L
        pendingAuthorize = false
        send(DiscordIpc.MSG_STOP)
        started = false
        status.value = FAILED
        error.value = message
        // Stop the 1 Hz IPC ping: there is nothing left to observe, and the state is terminal until
        // the user acts. authorize() re-enters start(), which relaunches this — `started` is false
        // above precisely so that re-entry is not swallowed. Safe to call from inside the loop: the
        // cancellation lands on the next delay().
        pollJob?.cancel()
        pollJob = null
    }

    /**
     * Deadline check for one poll tick. [snapshotStatus] is null when the helper has not answered
     * at all yet. Returns true when it gave up, so the caller stops applying the live status.
     */
    private fun checkStuck(snapshotStatus: Int?): Boolean {
        if (gaveUpMessage != null) return true

        val (limit, reason) = when (snapshotStatus) {
            null -> HELPER_TIMEOUT_MS to "friends.error.helper"
            AUTHORIZING -> AUTHORIZE_TIMEOUT_MS to "friends.error.authorize"
            CONNECTING -> CONNECT_TIMEOUT_MS to "friends.error.connect"
            // Anything else is a settled state: DISABLED, DISCONNECTED, CONNECTED, FAILED.
            else -> {
                nonTerminalSinceMs = 0L
                connectRetried = false
                return false
            }
        }

        val now = android.os.SystemClock.elapsedRealtime()
        if (nonTerminalSinceMs == 0L) {
            nonTerminalSinceMs = now
            return false
        }
        if (now - nonTerminalSinceMs < limit) return false

        // A wedged handshake with a token we already hold is worth exactly one clean retry — a
        // device coming back from sleep can land here through no fault of the user, and making
        // them press Connect for that would be worse than the wait. Sign-in has a human in it and
        // gets no retry: re-opening the browser under someone is not a recovery.
        if (snapshotStatus == CONNECTING && !connectRetried) {
            connectRetried = true
            nonTerminalSinceMs = now
            Log.w(TAG, "connect stalled past ${limit}ms — one retry")
            send(DiscordIpc.MSG_START, Bundle().apply { putString(DiscordIpc.DATA_TOKEN, savedToken) })
            return false
        }

        giveUp(com.armsx2.i18n.I18n.get(reason))
        return true
    }

    /** One free retry per stalled handshake; reset whenever the status settles. */
    private var connectRetried = false

    /**
     * Poll the bridge for status and friends.
     *
     * Polling rather than callbacks: the SDK fires on its own threads, so pushing into Kotlin would
     * need AttachCurrentThread and a global ref outliving the Activity. A one-second poll of two
     * cheap accessors costs nothing next to that, and this only runs while signed in.
     */
    private fun startPolling() {
        pollJob?.cancel()
        pollJob = scope.launch {
            var sinceRetry = 0
            var retryAfter = 5
            // RA rich presence changes AS YOU PLAY, unlike the game title -- so it cannot ride the
            // currentGame snapshotFlow alone. Re-checked here, but pushed only when the line really
            // changed and at most every RA_REPUSH_SECONDS: Discord rate-limits presence updates,
            // and some games update their RA line every few seconds.
            var sinceRaCheck = 0
            while (true) {
                // No pump here any more: the SDK's callback queue is drained inside the helper, on
                // the thread that created the client. This loop only asks for a snapshot.
                bindHelper()
                send(DiscordIpc.MSG_QUERY)
                delay(1000)

                val snap = lastState
                if (snap == null) {
                    // Helper not answering yet. Say "connecting" rather than "failed" — a bind
                    // takes a moment and the SDK is not the thing that is slow. But it is NOT
                    // allowed to say that forever: checkStuck flips to a real error once the
                    // helper has had long enough to come up.
                    if (checkStuck(null)) continue
                    if (enabled && status.value == DISABLED) status.value = CONNECTING
                    continue
                }

                helperAvailable = snap.getBoolean(DiscordIpc.DATA_AVAILABLE, true)
                val s = snap.getInt(DiscordIpc.DATA_STATUS, DISABLED)
                // Deadline for the states nothing on this side can end: once blown, the panel
                // stays on the failure (with its message) rather than reverting to a spinner the
                // moment the next snapshot lands.
                if (checkStuck(s)) continue
                status.value = s

                snap.getString(DiscordIpc.DATA_FRESH_TOKEN)?.takeIf { it.isNotBlank() }?.let { fresh ->
                    savedToken = fresh
                    Log.i(TAG, "authorization stored")
                }

                // Reconnect after a drop. The SDK does not retry, and its connection does not
                // survive the app being backgrounded for long — swapping apps, or a device
                // sleeping. MSG_START is idempotent in the helper, so it doubles as the retry.
                if (s == DISCONNECTED && enabled && savedToken.isNotBlank()) {
                    if (++sinceRetry >= retryAfter) {
                        sinceRetry = 0
                        retryAfter = (retryAfter * 2).coerceAtMost(60)
                        Log.i(TAG, "connection dropped — reconnecting (next retry in $retryAfter s)")
                        send(DiscordIpc.MSG_START, Bundle().apply {
                            putString(DiscordIpc.DATA_TOKEN, savedToken)
                        })
                    }
                } else if (s == CONNECTED) {
                    sinceRetry = 0
                    retryAfter = 5
                }

                // Only touch the friends list while actually connected. A drop reports nobody
                // online, and letting that through would clear the known set — so reconnecting
                // would then announce every friend as a fresh arrival.
                if (s == CONNECTED) {
                    val current = parseFriends(snap.getString(DiscordIpc.DATA_FRIENDS).orEmpty())
                    announceArrivals(current)
                    friends.value = current

                    if (self.value == null) {
                        val f = snap.getString(DiscordIpc.DATA_SELF).orEmpty().split(FIELD_SEP)
                        val name = f.getOrNull(0).orEmpty()
                        if (name.isNotBlank()) {
                            self.value = DiscordFriend(
                                name = name,
                                game = "",
                                serial = "",
                                avatarUrl = f.getOrNull(1).orEmpty(),
                            )
                        }
                    }
                }

                if (s == CONNECTED && ++sinceRaCheck >= RA_REPUSH_SECONDS) {
                    sinceRaCheck = 0
                    // Computed once and reused for the push: the throttle and the payload want the
                    // same line, and recomputing would double the work.
                    val game = MainActivityRuntime.currentGame.value
                    val line = raPresence(game)
                    // The game may have changed while that was computing off-thread — the watcher
                    // below has already published the new one, so do not clobber it with a line
                    // describing the old game. The next tick picks the new one up.
                    if (game == MainActivityRuntime.currentGame.value && line != lastRaPresence) {
                        sendPresence(game, line)
                    }
                }

                error.value = if (s == FAILED) snap.getString(DiscordIpc.DATA_ERROR) else null
            }
        }
    }

    /**
     * Tell the player when somebody joins, on the emulator's own OSD so it lands over the game
     * rather than requiring them to be looking at the Friends screen.
     *
     * Only ARRIVALS: diffing against the previous set means a friend who is simply still online
     * does not re-announce every second. Departures are deliberately silent — someone quitting is
     * not news worth covering the game for.
     *
     * The first poll after connecting seeds the set without announcing. Otherwise signing in would
     * dump one notification per friend already playing, which is a wall, not an alert.
     */
    private fun parseFriends(raw: String): List<DiscordFriend> {
        if (raw.isBlank()) return emptyList()
        return raw.split(RECORD_SEP).mapNotNull { record ->
            val f = record.split(FIELD_SEP)
            val name = f.getOrNull(0).orEmpty()
            if (name.isBlank()) return@mapNotNull null
            DiscordFriend(
                name = name,
                game = f.getOrNull(1).orEmpty(),
                serial = f.getOrNull(2).orEmpty(),
                avatarUrl = f.getOrNull(3).orEmpty(),
            )
        }
    }

    private fun announceArrivals(current: List<DiscordFriend>) {
        val currentSet = current.map { it.name }.toSet()
        val previous = knownFriends
        knownFriends = currentSet

        // Absorb the first poll after connecting without announcing: signing in should not dump one
        // notification per friend already playing.
        if (!seededFriends) {
            seededFriends = true
            return
        }
        if (!notifyInGame) return

        val arrivals = current.filterNot { it.name in previous }
        if (arrivals.isEmpty()) return

        // One surface for both cases. This used to branch: emulator OSD in a game, Compose banner
        // in the library. The OSD is text-only, so it could not show an avatar and looked nothing
        // like the library's version of the same event. The banner is composed at the Activity
        // root, so it draws over the game too and there is no reason to keep a second path.
        //
        // Last arrival wins if several land at once; a stack of banners over a game would be worse
        // than the one that is actually current.
        arrivals.forEach { friend -> justOnline.value = friend }
    }

    /**
     * Publish whatever is running.
     *
     * Watches [MainActivityRuntime.currentGame] instead of being called from the launch paths —
     * there are five places that assign it, and a snapshot flow catches every one of them, plus any
     * added later. A missed call site here would be invisible: presence would simply be stale.
     */
    private fun startPresenceWatch() {
        presenceJob?.cancel()
        presenceJob = scope.launch {
            // pushPresence reads currentGame itself, so this only needs to know it changed. The
            // cover it sends is the same URL the library renders — Discord takes a URL, so there
            // is nothing to upload per game and a title with no published cover falls back.
            snapshotFlow { MainActivityRuntime.currentGame.value }.collect { pushPresence() }
        }
    }
}
