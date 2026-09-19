package com.armsx2.input

import android.view.InputDevice
import android.view.MotionEvent

object ControllerMotion {
    private data class Axis(val id: Int, val source: Int, val range: ControllerAxisPolicy.Range)
    private data class Profile(
        val sources: Int,
        val ranges: List<Axis>,
        val rightStick: Pair<Int, Int>,
        val values: MutableMap<Int, Float> = HashMap(),
    )

    private val profiles = HashMap<Int, Profile>()

    private fun isControllerSource(source: Int): Boolean =
        source and InputDevice.SOURCE_JOYSTICK == InputDevice.SOURCE_JOYSTICK ||
            source and InputDevice.SOURCE_GAMEPAD == InputDevice.SOURCE_GAMEPAD ||
            source and InputDevice.SOURCE_DPAD == InputDevice.SOURCE_DPAD

    private fun profile(deviceId: Int): Profile? {
        profiles[deviceId]?.let { return it }
        val device = InputDevice.getDevice(deviceId) ?: return null
        val ranges = device.motionRanges.filter { isControllerSource(it.source) }.map {
            Axis(it.axis, it.source, ControllerAxisPolicy.Range(it.min, it.max, it.flat))
        }
        fun axis(id: Int) = ranges.firstOrNull { it.id == id }?.range
        val rotational = ControllerAxisPolicy.useRotationalRightStick(
            axis(MotionEvent.AXIS_Z), axis(MotionEvent.AXIS_RZ),
            axis(MotionEvent.AXIS_RX), axis(MotionEvent.AXIS_RY), device.vendorId == 0x057e,
        )
        return Profile(device.sources, ranges, if (rotational) MotionEvent.AXIS_RX to MotionEvent.AXIS_RY
            else MotionEvent.AXIS_Z to MotionEvent.AXIS_RZ).also { profiles[deviceId] = it }
    }

    fun invalidate(deviceId: Int) { profiles.remove(deviceId) }

    fun isControllerDevice(deviceId: Int): Boolean = profile(deviceId)?.sources?.let(::isControllerSource) == true

    fun resetValues() { profiles.values.forEach { it.values.clear() } }

    fun update(event: MotionEvent) {
        val profile = profile(event.deviceId) ?: return
        val dpadOnly = event.isFromSource(InputDevice.SOURCE_DPAD) &&
            !event.isFromSource(InputDevice.SOURCE_JOYSTICK) && !event.isFromSource(InputDevice.SOURCE_GAMEPAD)
        val hasExactSource = profile.ranges.any { it.source == event.source }
        for (axis in profile.ranges) {
            if (dpadOnly && axis.id != MotionEvent.AXIS_HAT_X && axis.id != MotionEvent.AXIS_HAT_Y) continue
            if (!dpadOnly && hasExactSource && axis.source != event.source) continue
            profile.values[axis.id] = event.getAxisValue(axis.id)
        }
    }

    fun isController(event: MotionEvent): Boolean {
        if (event.actionMasked != MotionEvent.ACTION_MOVE) return false
        val sourceClass = event.source and InputDevice.SOURCE_CLASS_MASK
        if (sourceClass == InputDevice.SOURCE_CLASS_POINTER ||
            sourceClass == InputDevice.SOURCE_CLASS_POSITION ||
            sourceClass == InputDevice.SOURCE_CLASS_TRACKBALL) return false
        return isControllerSource(event.source) ||
            (profile(event.deviceId)?.sources?.let(::isControllerSource) == true)
    }

    fun rightStickAxes(deviceId: Int): Pair<Int, Int> =
        profile(deviceId)?.rightStick ?: (MotionEvent.AXIS_Z to MotionEvent.AXIS_RZ)

    private fun range(event: MotionEvent, axis: Int): ControllerAxisPolicy.Range? {
        val ranges = profile(event.deviceId)?.ranges ?: return null
        return (ranges.firstOrNull { it.id == axis && it.source == event.source }
            ?: ranges.firstOrNull { it.id == axis })?.range
    }

    fun centered(event: MotionEvent, axis: Int): Float {
        if (axis < 0) return 0f
        val profile = profile(event.deviceId)
        val range = range(event, axis)
        val value = profile?.values?.get(axis) ?: if (range == null) event.getAxisValue(axis) else return 0f
        return ControllerAxisPolicy.centered(value, range)
    }

    fun trigger(event: MotionEvent, vararg axes: Int): Float = axes.maxOfOrNull { axis ->
        val range = range(event, axis)
        val value = profile(event.deviceId)?.values?.get(axis)
        if (axis < 0 || range == null || value == null) 0f else ControllerAxisPolicy.trigger(value, range)
    } ?: 0f

    fun hasAxis(event: MotionEvent, axis: Int): Boolean = axis >= 0 && range(event, axis) != null

    fun rightTriggerExtraAxis(deviceId: Int): Int {
        val profile = profile(deviceId) ?: return -1
        if (profile.rightStick.second == MotionEvent.AXIS_RZ) return -1
        val rz = profile.ranges.firstOrNull { it.id == MotionEvent.AXIS_RZ }?.range ?: return -1
        return if (rz.min >= 0f) MotionEvent.AXIS_RZ else -1
    }
}
