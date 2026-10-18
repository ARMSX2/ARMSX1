package com.armsx2.ui.settings

import android.content.Context
import android.hardware.input.InputManager
import android.os.Handler
import android.os.Looper
import android.view.InputDevice
import android.view.MotionEvent
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.*
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.unit.dp
import com.armsx2.i18n.str
import com.armsx2.input.ControllerAutoMapping
import com.armsx2.input.ControllerMappings
import com.armsx2.input.ControllerMotion

/** Device picker stays in the app's controller-navigation tree, not a separate Dialog window. */
@Composable
internal fun AutomaticMappingSection(player: Int, serial: String?, onApplied: () -> Unit) {
    val context = LocalContext.current
    val title = str("pad.autoMapping.label")
    var deviceRevision by remember { mutableIntStateOf(0) }
    var result by remember(player, serial) { mutableStateOf<String?>(null) }
    val success = str("pad.autoMapping.result")
    val unsupported = str("pad.autoMapping.unsupported")
    val disconnected = str("pad.autoMapping.disconnected")
    CollapsibleSection(title) {
        DisposableEffect(context) {
            val manager = context.getSystemService(Context.INPUT_SERVICE) as InputManager
            val listener = object : InputManager.InputDeviceListener {
                override fun onInputDeviceAdded(deviceId: Int) { deviceRevision++ }
                override fun onInputDeviceRemoved(deviceId: Int) { deviceRevision++ }
                override fun onInputDeviceChanged(deviceId: Int) { deviceRevision++ }
            }
            manager.registerInputDeviceListener(listener, Handler(Looper.getMainLooper()))
            onDispose { manager.unregisterInputDeviceListener(listener) }
        }
        val devices = remember(deviceRevision) {
            InputDevice.getDeviceIds().map { InputDevice.getDevice(it) }.filterNotNull().filter {
                !it.isVirtual && (it.supportsSource(InputDevice.SOURCE_GAMEPAD) || it.supportsSource(InputDevice.SOURCE_JOYSTICK))
            }.sortedWith(compareBy({ it.name }, { it.id }))
        }
        HelpText(str("pad.autoMapping.description"))
        if (devices.isEmpty()) HelpText(str("pad.autoMapping.noDevices"))
        devices.forEach { device ->
            val apply = {
                val current = InputDevice.getDevice(device.id)
                result = if (current == null || current.descriptor != device.descriptor) disconnected else {
                    val plan = ControllerAutoMapping.plan(capabilities(current))
                    if (ControllerMappings.applyAutomaticMapping(plan, player, serial)) {
                        onApplied()
                        success.format(current.name, plan.mappedButtons)
                    } else unsupported
                }
            }
            Column(Modifier.fillMaxWidth()
                .controllerFocusable("pad-auto-map:$player:${device.id}", onConfirm = { apply() })
                .clickable { apply() }.padding(horizontal = 12.dp, vertical = 14.dp)) {
                Text(str("pad.autoMapping.device").format(device.name), color = MaterialTheme.colorScheme.primary)
            }
        }
        result?.let { HelpText(it) }
    }
    SettingsDivider()
}

private fun capabilities(device: InputDevice): ControllerAutoMapping.Capabilities {
    val codes = ControllerAutoMapping.standardButtons.values.distinct().toIntArray()
    val has = device.hasKeys(*codes)
    fun axis(id: Int) = device.motionRanges.any {
        it.axis == id && it.range > 0f &&
            ((it.source and InputDevice.SOURCE_JOYSTICK) == InputDevice.SOURCE_JOYSTICK ||
                (it.source and InputDevice.SOURCE_GAMEPAD) == InputDevice.SOURCE_GAMEPAD)
    }
    val rightAxes = ControllerMotion.rightStickAxes(device.id)
    return ControllerAutoMapping.Capabilities(
        keys = codes.indices.filter { has[it] }.map { codes[it] }.toSet(),
        hatX = axis(MotionEvent.AXIS_HAT_X), hatY = axis(MotionEvent.AXIS_HAT_Y),
        leftTrigger = axis(MotionEvent.AXIS_LTRIGGER) || axis(MotionEvent.AXIS_BRAKE),
        rightTrigger = axis(MotionEvent.AXIS_RTRIGGER) || axis(MotionEvent.AXIS_GAS) ||
            ControllerMotion.rightTriggerExtraAxis(device.id) >= 0,
        leftStick = axis(MotionEvent.AXIS_X) && axis(MotionEvent.AXIS_Y),
        rightStick = axis(rightAxes.first) && axis(rightAxes.second),
    )
}
