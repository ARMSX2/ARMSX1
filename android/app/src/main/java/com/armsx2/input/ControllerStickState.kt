package com.armsx2.input

class ControllerStickState(players: Int) {
    private val physical = Array(players) { Array(2) { IntArray(4) } }
    private val touch = Array(players) { Array(2) { IntArray(4) } }

    fun setPhysicalDirection(port: Int, stick: Int, direction: Int, value: Int): Int {
        physical[port][stick][direction] = value.coerceIn(0, 32767)
        return merged(port, stick)
    }

    fun setPhysicalStick(port: Int, stick: Int, xPos: Int, xNeg: Int, yPos: Int, yNeg: Int): Int {
        set(physical[port][stick], xPos, xNeg, yPos, yNeg)
        return merged(port, stick)
    }

    fun setTouchStick(port: Int, stick: Int, xPos: Int, xNeg: Int, yPos: Int, yNeg: Int): Int {
        set(touch[port][stick], xPos, xNeg, yPos, yNeg)
        return merged(port, stick)
    }

    fun merged(port: Int, stick: Int): Int {
        val p = physical[port][stick]
        val t = touch[port][stick]
        val x = ControllerStickResponse.axisByte(maxOf(p[1], t[1]), maxOf(p[3], t[3]))
        val y = ControllerStickResponse.axisByte(maxOf(p[2], t[2]), maxOf(p[0], t[0]))
        return x or (y shl 8)
    }

    fun reset() {
        for (source in arrayOf(physical, touch))
            for (player in source)
                for (stick in player) stick.fill(0)
    }

    private fun set(state: IntArray, xPos: Int, xNeg: Int, yPos: Int, yNeg: Int) {
        state[0] = yNeg.coerceIn(0, 32767)
        state[1] = xPos.coerceIn(0, 32767)
        state[2] = yPos.coerceIn(0, 32767)
        state[3] = xNeg.coerceIn(0, 32767)
    }
}
