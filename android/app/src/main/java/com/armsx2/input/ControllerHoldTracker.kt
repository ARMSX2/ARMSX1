package com.armsx2.input

class ControllerHoldTracker {
    enum class Action { FAST_FORWARD, REWIND, GYRO }

    private data class Key(val deviceId: Int, val code: Int)
    private val held = HashMap<Key, Action>()
    private val requiredKeys = HashMap<Key, Set<Int>>()

    fun press(deviceId: Int, code: Int, action: Action, codes: Set<Int> = setOf(code)): Boolean {
        val key = Key(deviceId, code)
        if (key in held) return false
        val first = action !in held.values
        held[key] = action
        requiredKeys[key] = codes
        return first
    }

    fun release(deviceId: Int, code: Int, onRelease: (Action) -> Unit): Boolean {
        val keys = held.keys.filter { it.deviceId == deviceId && code in requiredKeys.getValue(it) }
        if (keys.isEmpty()) return false
        val actions = keys.mapNotNull { held.remove(it) }.toSet()
        keys.forEach { requiredKeys.remove(it) }
        actions.filter { it !in held.values }.forEach(onRelease)
        return true
    }

    fun cancel(action: Action) {
        val entries = held.entries.iterator()
        while (entries.hasNext()) {
            val entry = entries.next()
            if (entry.value == action) { requiredKeys.remove(entry.key); entries.remove() }
        }
    }

    fun releaseDevice(deviceId: Int, onRelease: (Action) -> Unit) {
        val keys = held.keys.filter { it.deviceId == deviceId }
        keys.forEach { release(it.deviceId, it.code, onRelease) }
    }

    fun reset(onRelease: (Action) -> Unit) {
        val actions = held.values.toSet()
        held.clear()
        requiredKeys.clear()
        actions.forEach(onRelease)
    }
}
