import com.armsx2.input.ControllerHoldTracker
import com.armsx2.input.ControllerHoldTracker.Action

fun testControllerHolds() {
    val holds = ControllerHoldTracker()
    val released = mutableListOf<Action>()
    val release = { action: Action -> released.add(action); Unit }

    check(holds.press(1, 20, Action.FAST_FORWARD))
    check(!holds.release(1, 10, release))
    check(released.isEmpty())
    check(holds.release(1, 20, release))
    check(released == listOf(Action.FAST_FORWARD))
    check(!holds.release(1, 20, release))
    check(!holds.release(2, 20, release))
    check(released.size == 1)

    released.clear()
    check(holds.press(1, 20, Action.REWIND))
    check(!holds.press(1, 20, Action.REWIND))
    check(!holds.press(1, 20, Action.GYRO))
    check(holds.release(1, 20, release))
    check(!holds.release(1, 10, release))
    check(released == listOf(Action.REWIND))

    released.clear()
    check(holds.press(1, 20, Action.FAST_FORWARD))
    check(!holds.press(2, 20, Action.FAST_FORWARD))
    check(!holds.release(3, 20, release))
    check(holds.release(1, 20, release))
    check(released.isEmpty())
    check(holds.release(2, 20, release))
    check(released == listOf(Action.FAST_FORWARD))

    released.clear()
    check(holds.press(1, 20, Action.GYRO))
    check(!holds.press(2, 20, Action.GYRO))
    check(holds.press(1, 21, Action.REWIND))
    holds.releaseDevice(1, release)
    check(released == listOf(Action.REWIND))
    holds.releaseDevice(1, release)
    check(released.size == 1)
    holds.releaseDevice(2, release)
    check(released == listOf(Action.REWIND, Action.GYRO))

    released.clear()
    check(holds.press(1, 20, Action.GYRO))
    check(!holds.press(2, 20, Action.GYRO))
    check(holds.press(1, 21, Action.FAST_FORWARD))
    check(holds.press(1, 22, Action.REWIND))
    holds.reset(release)
    check(released.toSet() == Action.values().toSet())
    check(released.size == 3)
    holds.reset(release)
    check(!holds.release(1, 20, release))
    check(released.size == 3)

    released.clear()
    check(holds.press(1, 20, Action.GYRO))
    check(holds.press(1, 21, Action.FAST_FORWARD))
    holds.cancel(Action.GYRO)
    holds.cancel(Action.FAST_FORWARD)
    check(!holds.release(1, 20, release))
    check(!holds.release(1, 21, release))
    holds.reset(release)
    check(released.isEmpty())

    check(holds.press(1, 20, Action.GYRO))
    var gyroEnabled = true
    val releaseGyro = { action: Action -> if (action == Action.GYRO) gyroEnabled = false }
    holds.cancel(Action.GYRO)
    gyroEnabled = true
    holds.reset(releaseGyro)
    check(!holds.release(1, 20, releaseGyro))
    check(gyroEnabled)
    check(holds.press(1, 20, Action.GYRO))
    holds.release(1, 20, releaseGyro)
    check(!gyroEnabled)
    println("Controller held-action release, reset, device ownership and toggle tests passed")
}
