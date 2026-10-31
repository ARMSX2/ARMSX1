import com.armsx2.input.ControllerStickResponse
import kotlin.math.abs
import kotlin.math.sqrt

fun testControllerStickResponse() {
    fun shaped(x: Float, y: Float, sensitivity: Float = 1.33f, outer: Float = 0f,
               acceleration: Float = 0f, gamma: Float = 0f, anti: Float = 0f) =
        ControllerStickResponse.vector(x, y, 0.05f, outer, acceleration, gamma, sensitivity, anti)
    fun near(a: Float, b: Float) = check(abs(a - b) < 0.0001f) { "$a != $b" }
    check(shaped(0f, 0f) == (0f to 0f))
    check(shaped(0.03f, 0.03f) == (0f to 0f))
    check(shaped(Float.NaN, 1f) == (0f to 0f))
    check(shaped(1f, 0f) == (1f to 0f))
    check(shaped(-1f, 0f) == (-1f to 0f))
    check(shaped(0f, 1f) == (0f to 1f))
    check(shaped(0f, -1f) == (0f to -1f))
    val corner = 1f / sqrt(2f)
    for (sx in listOf(-1f, 1f)) for (sy in listOf(-1f, 1f)) {
        val diagonal = shaped(sx * corner, sy * corner)
        near(abs(diagonal.first), corner * 1.33f)
        near(abs(diagonal.second), corner * 1.33f)
        check(shaped(sx, sy) == (sx to sy))
    }
    near(shaped(0.5f, 0f, sensitivity = 1f).first, 0.45f / 0.95f)
    check(shaped(0.5f, 0f).first > shaped(0.5f, 0f, sensitivity = 1f).first)
    near(shaped(0.8f, 0f, sensitivity = 1f, outer = 0.2f).first, 1f)
    check(shaped(0.5f, 0f, acceleration = 1f).first < shaped(0.5f, 0f).first)
    check(shaped(0.5f, 0f, gamma = 1f).first < shaped(0.5f, 0f).first)
    check(shaped(0.1f, 0f, anti = 0.3f).first >= 0.3f)
    check(shaped(0f, 0f, anti = 0.3f) == (0f to 0f))
    near(shaped(1f, 0.1f).second, 0f)
    check(ControllerStickResponse.axisByte(0, 0) == 128)
    check(ControllerStickResponse.axisByte(32767, 0) == 255)
    check(ControllerStickResponse.axisByte(0, 32767) == 0)
    check(ControllerStickResponse.axisByte(32767, 32767) == 128)
    var previous = -1
    for (delta in -32767..32767) {
        val byte = ControllerStickResponse.axisByte(delta.coerceAtLeast(0), (-delta).coerceAtLeast(0))
        check(byte in 0..255 && byte >= previous)
        previous = byte
    }
    println("Controller stick response passed: endpoints, diagonals, feel settings, 65535 axis values")
}
