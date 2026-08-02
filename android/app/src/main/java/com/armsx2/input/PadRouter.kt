package com.armsx2.input

import android.view.InputDevice

/**
 * Auto-assigns physical controllers to PS1 pad slots.
 *
 * First distinct gamepad = Player 1 (slot 0); the next = Player 2 (slot 1, hot-plugged via
 * [onPlayer2Joined]); any further pads fold into Player 1.
 *
 * PS1 PORT: there is no "2 ports x 4 taps" tier. The PlayStation Multitap puts FOUR pads
 * behind ONE controller port, so the claimable slots are 0..3 and they are the tap's
 * A/B/C/D — see psx/input/multitap.c. With the tap off only slot 0 exists, because the core
 * drops any player above 0 when the port holds a plain pad (psx_pad_button_press_player).
 * The backing array is kept at 8 entries only so the per-slot scratch arrays in
 * MainActivityRuntime (which are `Array(8)`) stay index-compatible — slots 4..7 are never
 * claimed.
 *
 * The on-screen touch controls and all menu navigation always use slot 0 — they never go
 * through here. Reset on VM start AND stop ([reset]) so each session re-pairs
 * deterministically.
 *
 * Called only from the in-game input dispatch where a real `event.deviceId` is live.
 */
object PadRouter {
    // Nintendo USB/Bluetooth vendor id. A Joy-Con pair enumerates as TWO InputDevices
    // (L + R) under this vendor, but they are one physical controller — so both halves
    // are routed to a single port instead of being split across players.
    private const val NINTENDO_VENDOR_ID = 0x057E
    // Unified slot -> claimed Android deviceId (-1 = unclaimed). Sized 8 for index
    // compatibility with MainActivityRuntime's per-slot Array(8) scratch state; only the
    // first maxSlots() entries are ever claimed on this port.
    private val slots = IntArray(8) { -1 }
    @Volatile private var pad2Enabled = false

    /**
     * Claimable slots RIGHT NOW.
     *
     * One without a Multitap, four with one. It is derived rather than constant because a
     * claim that has nowhere to land is a black hole: before the tap existed, the second
     * controller to enumerate (very common on a handheld — built-in pad takes slot 0, a
     * paired Bluetooth pad takes slot 1) claimed a slot the core dropped every input for,
     * and simply went dead. Anything past the cap folds onto Player 1, which is always a
     * destination that exists.
     */
    private fun maxSlots(): Int = if (multitapEnabled) 4 else 1

    /** Whether controller port 1 currently holds a Multitap.
     *
     *  Seeded at startup from `[input] multitap` and updated by the Multitap toggles; the
     *  matching native (NativeApp.setMultitapEnabled) swaps the device in the core. Keeping
     *  the two in step is what makes players 2-4 claimable at the moment the tap is plugged
     *  in — this used to be a permanently-false stub, so extra pads had nowhere to go. */
    @Volatile var multitapEnabled = false
        private set

    /** Turn the tap tier on or off. Slots above the new cap are released so a pad that was
     *  parked on (say) slot 3 re-claims from the top instead of staying somewhere the core
     *  no longer reads. Does NOT touch the core — the caller pushes
     *  NativeApp.setMultitapEnabled / persists `[input] multitap` itself. */
    fun setMultitapEnabled(enabled: Boolean) {
        if (multitapEnabled == enabled) return
        multitapEnabled = enabled
        val cap = maxSlots()
        for (i in cap until slots.size) slots[i] = -1
    }

    /** Fired exactly once, the first time the 2nd controller (slot 1 = P2 main) joins,
     *  so the app can hot-plug the native Pad2 slot before any P2 input is sent. Tap
     *  slots (2-7) don't use this — they're armed at boot / by the Multitap toggle. */
    @Volatile var onPlayer2Joined: (() -> Unit)? = null

    fun reset() {
        for (i in slots.indices) slots[i] = -1
        pad2Enabled = false
    }

    /** Release the slot a now-departed device held, so a re-enumerated controller re-claims from the
     *  top. Called on device removal: AYANEO handhelds power-cycle the built-in pad on sleep/wake and
     *  it comes back with a NEW deviceId — the stale id otherwise keeps owning Player 1's slot, and
     *  the woken pad claims slot 1 (an un-armed PS2 port 2), so gameplay input goes nowhere (#394). */
    fun forgetDevice(deviceId: Int) {
        if (deviceId < 0) return
        for (i in slots.indices) if (slots[i] == deviceId) slots[i] = -1
    }

    /** Free every slot whose claimed device is no longer connected. Called on resume / focus regain
     *  as a backstop for [forgetDevice] when the remove event landed while we were paused.
     *  [activeDeviceIds] is `InputDevice.getDeviceIds()`. */
    fun pruneStale(activeDeviceIds: IntArray) {
        for (i in slots.indices) {
            val id = slots[i]
            if (id >= 0 && id !in activeDeviceIds) slots[i] = -1
        }
    }

    /** True once a second controller has joined this session (P2 main is live). */
    fun coopActive(): Boolean = slots[1] != -1

    /** Players currently holding a slot. 1 with a lone pad, up to 4 behind a Multitap. */
    fun activePlayers(): Int = (0 until maxSlots()).count { slots[it] != -1 }.coerceAtLeast(1)

    /** Android InputDevice id assigned to a pad slot, or -1 if unclaimed.
     *  Lets per-slot rumble buzz the right pad. */
    fun deviceIdForPort(port: Int): Int =
        if (port in slots.indices) slots[port] else -1

    /**
     * Map a physical input device to a PS1 pad slot, claiming the next free one.
     * Synthetic / virtual events (deviceId < 0) and non-gamepad nodes never claim a slot —
     * they're treated as Player 1. A pad past the current cap folds into Player 1.
     */
    fun portForDevice(deviceId: Int): Int {
        if (deviceId < 0) return 0
        // Fast path: already-claimed nodes (no InputDevice lookup).
        for (i in slots.indices) if (slots[i] == deviceId) return i
        val dev = InputDevice.getDevice(deviceId)
        // Nintendo Joy-Cons (vendor 0x057E) enumerate as TWO InputDevices — the L and R
        // halves of ONE physical controller. Collapse BOTH onto Player 1 (port 0) so a
        // pair drives a single PS1 pad instead of splitting across P1/P2 (which made a
        // 1-player game respond to only one half, and a co-op game see two controllers).
        // They never claim a slot, so onPlayer2Joined stays silent for a lone pair. Gated
        // strictly on the Nintendo vendor id — every other controller keeps its routing.
        if (dev?.vendorId == NINTENDO_VENDOR_ID) return 0
        // Only a real GAMEPAD/JOYSTICK node may claim a slot. One physical controller
        // (notably a DualSense over Bluetooth) enumerates as SEVERAL InputDevices — a
        // gamepad node PLUS a touchpad/mouse node. Gating claims to gamepad sources stops
        // a secondary node from eating a slot and splitting one pad across two players.
        val src = dev?.sources ?: 0
        val isPad = (src and InputDevice.SOURCE_GAMEPAD) == InputDevice.SOURCE_GAMEPAD ||
            (src and InputDevice.SOURCE_JOYSTICK) == InputDevice.SOURCE_JOYSTICK
        if (!isPad) return 0 // touchpad / mouse / keyboard node → treat as P1, don't claim
        for (i in 0 until maxSlots()) {
            if (slots[i] == -1) {
                slots[i] = deviceId
                if (i == 1 && !pad2Enabled) { pad2Enabled = true; onPlayer2Joined?.invoke() }
                return i
            }
        }
        return 0 // all slots taken -> fold into Player 1
    }
}
