import android.view.InputDevice
import android.view.MotionEvent
import com.armsx2.input.ControllerAxisPolicy
import com.armsx2.input.ControllerMotion
import kotlin.math.abs

private fun equal(actual: Float, expected: Float) {
    check(abs(actual - expected) < 0.0001f) { "Expected $expected, got $actual" }
}

private fun axis(id: Int, min: Float = -1f, max: Float = 1f, source: Int = InputDevice.SOURCE_JOYSTICK) =
    InputDevice.MotionRange(id, source, min, max)

private fun motion(id: Int, source: Int = InputDevice.SOURCE_JOYSTICK, vararg values: Pair<Int, Float>): MotionEvent =
    MotionEvent(id, source, values = values.toMap()).also { check(ControllerMotion.isController(it)); ControllerMotion.update(it) }

fun main() {
    val signed = ControllerAxisPolicy.Range(-1f, 1f, 0.1f)
    equal(ControllerAxisPolicy.centered(0.05f, signed), 0f)
    equal(ControllerAxisPolicy.centered(-1f, signed), -1f)
    equal(ControllerAxisPolicy.centered(1f, signed), 1f)
    equal(ControllerAxisPolicy.centered(Float.NaN, signed), 0f)
    equal(ControllerAxisPolicy.centered(Float.POSITIVE_INFINITY, signed), 0f)
    equal(ControllerAxisPolicy.centered(0f, ControllerAxisPolicy.Range(0f, 0f)), 0f)
    equal(ControllerAxisPolicy.centered(127.5f, ControllerAxisPolicy.Range(0f, 255f)), 0f)
    equal(ControllerAxisPolicy.centered(0f, ControllerAxisPolicy.Range(0f, 255f)), -1f)
    equal(ControllerAxisPolicy.centered(255f, ControllerAxisPolicy.Range(0f, 255f)), 1f)
    equal(ControllerAxisPolicy.centered(-32768f, ControllerAxisPolicy.Range(-32768f, 32767f)), -1f)
    equal(ControllerAxisPolicy.centered(32767f, ControllerAxisPolicy.Range(-32768f, 32767f)), 1f)
    equal(ControllerAxisPolicy.trigger(-1f, signed), 0f)
    equal(ControllerAxisPolicy.trigger(0f, signed), 0.5f)
    equal(ControllerAxisPolicy.trigger(1f, signed), 1f)
    check(ControllerAxisPolicy.useRotationalRightStick(ControllerAxisPolicy.Range(0f, 1f),
        ControllerAxisPolicy.Range(0f, 1f), ControllerAxisPolicy.Range(0f, 255f),
        ControllerAxisPolicy.Range(0f, 255f), false))

    val standard = listOf(axis(MotionEvent.AXIS_X), axis(MotionEvent.AXIS_Y), axis(MotionEvent.AXIS_Z),
        axis(MotionEvent.AXIS_RZ), axis(MotionEvent.AXIS_HAT_X), axis(MotionEvent.AXIS_HAT_Y))
    InputDevice.devices[1] = InputDevice(InputDevice.SOURCE_JOYSTICK or InputDevice.SOURCE_DPAD or InputDevice.SOURCE_MOUSE, 0, standard)
    val stick = motion(1, values = arrayOf(MotionEvent.AXIS_X to 1f, MotionEvent.AXIS_Y to -1f))
    equal(ControllerMotion.centered(stick, MotionEvent.AXIS_X), 1f)
    equal(ControllerMotion.centered(stick, MotionEvent.AXIS_Y), -1f)
    check(ControllerMotion.rightStickAxes(1) == MotionEvent.AXIS_Z to MotionEvent.AXIS_RZ)
    check(ControllerMotion.rightTriggerExtraAxis(1) == -1)
    val hat = motion(1, InputDevice.SOURCE_DPAD, MotionEvent.AXIS_HAT_X to -1f)
    equal(ControllerMotion.centered(hat, MotionEvent.AXIS_X), 1f)
    equal(ControllerMotion.centered(hat, MotionEvent.AXIS_HAT_X), -1f)
    val neutralHat = motion(1, InputDevice.SOURCE_DPAD)
    equal(ControllerMotion.centered(neutralHat, MotionEvent.AXIS_HAT_X), 0f)
    equal(ControllerMotion.centered(neutralHat, MotionEvent.AXIS_X), 1f)
    check(!ControllerMotion.isController(MotionEvent(1, InputDevice.SOURCE_MOUSE)))
    check(!ControllerMotion.isController(MotionEvent(1, InputDevice.SOURCE_JOYSTICK, MotionEvent.ACTION_HOVER_MOVE)))
    check(ControllerMotion.isController(MotionEvent(1, InputDevice.SOURCE_KEYBOARD)))
    check(ControllerMotion.isController(MotionEvent(1, 0)))

    val rotated = listOf(axis(MotionEvent.AXIS_X, 0f, 255f), axis(MotionEvent.AXIS_Y, 0f, 255f),
        axis(MotionEvent.AXIS_Z, 0f, 1f), axis(MotionEvent.AXIS_RZ, 0f, 1f),
        axis(MotionEvent.AXIS_RX), axis(MotionEvent.AXIS_RY), axis(MotionEvent.AXIS_LTRIGGER))
    InputDevice.devices[2] = InputDevice(InputDevice.SOURCE_GAMEPAD or InputDevice.SOURCE_JOYSTICK, 0, rotated)
    val differentRanges = motion(2, values = arrayOf(MotionEvent.AXIS_X to 255f, MotionEvent.AXIS_Y to 127.5f,
        MotionEvent.AXIS_RX to -0.75f, MotionEvent.AXIS_RY to 0.5f, MotionEvent.AXIS_RZ to 0.8f, MotionEvent.AXIS_LTRIGGER to -1f))
    check(ControllerMotion.rightStickAxes(2) == MotionEvent.AXIS_RX to MotionEvent.AXIS_RY)
    check(ControllerMotion.rightTriggerExtraAxis(2) == MotionEvent.AXIS_RZ)
    equal(ControllerMotion.centered(differentRanges, MotionEvent.AXIS_X), 1f)
    equal(ControllerMotion.centered(differentRanges, MotionEvent.AXIS_Y), 0f)
    equal(ControllerMotion.centered(differentRanges, MotionEvent.AXIS_RX), -0.75f)
    equal(ControllerMotion.trigger(differentRanges, MotionEvent.AXIS_RZ), 0.8f)
    equal(ControllerMotion.trigger(differentRanges, MotionEvent.AXIS_LTRIGGER), 0f)
    equal(ControllerMotion.trigger(differentRanges, MotionEvent.AXIS_RTRIGGER), 0f)
    check(!ControllerMotion.hasAxis(differentRanges, MotionEvent.AXIS_RTRIGGER))

    InputDevice.devices[2] = InputDevice(InputDevice.SOURCE_JOYSTICK, 0, standard)
    ControllerMotion.invalidate(2)
    check(ControllerMotion.rightStickAxes(2) == MotionEvent.AXIS_Z to MotionEvent.AXIS_RZ)
    equal(ControllerMotion.centered(motion(2, values = arrayOf(MotionEvent.AXIS_X to -0.5f)), MotionEvent.AXIS_X), -0.5f)
    ControllerMotion.resetValues()
    equal(ControllerMotion.centered(neutralHat, MotionEvent.AXIS_X), 0f)
    InputDevice.devices.remove(2)
    ControllerMotion.invalidate(2)
    check(!ControllerMotion.isControllerDevice(2))

    InputDevice.devices[3] = InputDevice(InputDevice.SOURCE_JOYSTICK, 0,
        listOf(axis(MotionEvent.AXIS_Z), axis(MotionEvent.AXIS_RX), axis(MotionEvent.AXIS_RY)))
    check(ControllerMotion.rightStickAxes(3) == MotionEvent.AXIS_RX to MotionEvent.AXIS_RY)
    InputDevice.devices[4] = InputDevice(InputDevice.SOURCE_JOYSTICK, 0x057e, standard + axis(MotionEvent.AXIS_RX))
    check(ControllerMotion.rightStickAxes(4) == MotionEvent.AXIS_Z to MotionEvent.AXIS_RZ)
    InputDevice.devices[5] = InputDevice(InputDevice.SOURCE_JOYSTICK, 0x057e,
        standard + axis(MotionEvent.AXIS_RX) + axis(MotionEvent.AXIS_RY))
    check(ControllerMotion.rightStickAxes(5) == MotionEvent.AXIS_RX to MotionEvent.AXIS_RY)
    InputDevice.devices[6] = InputDevice(InputDevice.SOURCE_KEYBOARD, 0, emptyList())
    check(!ControllerMotion.isController(MotionEvent(6, InputDevice.SOURCE_KEYBOARD)))
    InputDevice.devices[7] = InputDevice(InputDevice.SOURCE_JOYSTICK or InputDevice.SOURCE_DPAD, 0,
        listOf(axis(MotionEvent.AXIS_X), axis(MotionEvent.AXIS_Y),
            axis(MotionEvent.AXIS_HAT_X, source = InputDevice.SOURCE_DPAD),
            axis(MotionEvent.AXIS_HAT_Y, source = InputDevice.SOURCE_DPAD)))
    motion(7, InputDevice.SOURCE_DPAD, MotionEvent.AXIS_HAT_Y to 1f)
    val splitSources = motion(7, values = arrayOf(MotionEvent.AXIS_X to 0.5f))
    equal(ControllerMotion.centered(splitSources, MotionEvent.AXIS_HAT_Y), 1f)
    equal(ControllerMotion.centered(splitSources, MotionEvent.AXIS_X), 0.5f)
    val absent = MotionEvent(8, InputDevice.SOURCE_JOYSTICK, values = mapOf(MotionEvent.AXIS_X to 0.75f))
    equal(ControllerMotion.centered(absent, MotionEvent.AXIS_X), 0.75f)
    InputDevice.devices[8] = InputDevice(InputDevice.SOURCE_JOYSTICK, 0, standard)
    ControllerMotion.update(absent)
    equal(ControllerMotion.centered(absent, MotionEvent.AXIS_X), 0.75f)
    println("Controller motion source, range, axis-layout and lifecycle tests passed")
}
