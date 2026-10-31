package android.view

class InputDevice(val sources: Int, val vendorId: Int, val motionRanges: List<MotionRange>) {
    data class MotionRange(val axis: Int, val source: Int, val min: Float, val max: Float, val flat: Float = 0f)

    companion object {
        const val SOURCE_CLASS_MASK = 0xff
        const val SOURCE_CLASS_POINTER = 2
        const val SOURCE_CLASS_TRACKBALL = 4
        const val SOURCE_CLASS_POSITION = 8
        const val SOURCE_DPAD = 0x201
        const val SOURCE_GAMEPAD = 0x401
        const val SOURCE_JOYSTICK = 0x1000010
        const val SOURCE_MOUSE = 0x2002
        const val SOURCE_KEYBOARD = 0x101
        val devices = HashMap<Int, InputDevice>()
        fun getDevice(id: Int): InputDevice? = devices[id]
    }
}

class MotionEvent(val deviceId: Int, val source: Int, val actionMasked: Int = ACTION_MOVE,
    private val values: Map<Int, Float> = emptyMap()) {
    fun isFromSource(expected: Int) = source and expected == expected
    fun getAxisValue(axis: Int) = values[axis] ?: 0f

    companion object {
        const val ACTION_MOVE = 2
        const val ACTION_HOVER_MOVE = 7
        const val AXIS_X = 0
        const val AXIS_Y = 1
        const val AXIS_Z = 11
        const val AXIS_RX = 12
        const val AXIS_RY = 13
        const val AXIS_RZ = 14
        const val AXIS_HAT_X = 15
        const val AXIS_HAT_Y = 16
        const val AXIS_LTRIGGER = 17
        const val AXIS_RTRIGGER = 18
    }
}
