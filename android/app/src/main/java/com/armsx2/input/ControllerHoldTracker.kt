package com.armsx2.input

class ControllerHoldTracker {
    enum class Action { FAST_FORWARD, REWIND, GYRO }

    private data class Key(val deviceId: Int, val code: Int)
    private val held = HashMap<Key, Action>()

    fun press(deviceId: Int, code: Int, action: Action): Boolean {
        val key = Key(deviceId, code)
        if (key in held) return false
        val first = action !in held.values
        held[key] = action
        return first
    }

    fun release(deviceId: Int, code: Int, onRelease: (Action) -> Unit): Boolean {
        val action = held.remove(Key(deviceId, code)) ?: return false
        if (action !in held.values) onRelease(action)
        return true
    }

    fun cancel(action: Action) {
        val entries = held.entries.iterator()
        while (entries.hasNext()) if (entries.next().value == action) entries.remove()
    }

    fun releaseDevice(deviceId: Int, onRelease: (Action) -> Unit) {
        val keys = held.keys.filter { it.deviceId == deviceId }
        keys.forEach { release(it.deviceId, it.code, onRelease) }
    }

    fun reset(onRelease: (Action) -> Unit) {
        val actions = held.values.toSet()
        held.clear()
        actions.forEach(onRelease)
    }
}
