import com.armsx2.input.ControllerStickState

fun testControllerStickState() {
    val state = ControllerStickState(4)
    fun expect(packed: Int, x: Int, y: Int) =
        check(packed == (x or (y shl 8))) { "axes=$packed expected=${x or (y shl 8)}" }
    expect(state.setTouchStick(0, 0, 32767, 0, 0, 0), 255, 128)
    expect(state.setPhysicalStick(0, 0, 32767, 0, 32767, 0), 255, 255)
    expect(state.setTouchStick(0, 0, 0, 0, 0, 0), 255, 255)
    expect(state.setTouchStick(0, 0, 0, 0, 0, 32767), 255, 128)
    expect(state.setPhysicalStick(0, 0, 0, 0, 0, 0), 128, 0)
    expect(state.setPhysicalDirection(0, 0, 3, 32767), 0, 0)
    expect(state.setTouchStick(0, 0, 32767, 0, 0, 32767), 128, 0)
    expect(state.setPhysicalDirection(0, 0, 3, 0), 255, 0)
    expect(state.setTouchStick(0, 0, 0, 0, 0, 0), 128, 128)
    expect(state.setPhysicalStick(0, 1, 0, 32767, 0, 0), 0, 128)
    expect(state.setTouchStick(0, 0, 32767, 0, 0, 0), 255, 128)
    expect(state.setPhysicalStick(3, 1, 0, 0, 32767, 0), 128, 255)
    expect(state.merged(0, 1), 0, 128)
    expect(state.merged(3, 0), 128, 128)
    state.setTouchStick(0, 0, 0, 0, 0, 0)
    expect(state.merged(3, 1), 128, 255)
    expect(state.setPhysicalStick(1, 0, 99999, -1, 0, 99999), 255, 0)
    state.reset()
    for (port in 0..3) for (stick in 0..1) expect(state.merged(port, stick), 128, 128)
    expect(state.setPhysicalDirection(0, 0, 1, 16384), 191, 128)
    expect(state.setTouchStick(0, 0, 32767, 0, 0, 0), 255, 128)
    expect(state.setTouchStick(0, 0, 0, 0, 0, 0), 191, 128)
    println("Controller stick sources passed: handoffs, opposed directions, reset, ports, legacy updates")
}
