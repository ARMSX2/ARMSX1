package com.armsx2.config

import android.content.Context
import android.content.Intent
import android.graphics.Bitmap
import android.os.SystemClock
import android.view.accessibility.AccessibilityNodeInfo
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import com.armsx2.SettingsChangeAuditActivity
import com.armsx2.core.Ps1Shaders
import com.armsx2.runtime.MainActivityRuntime
import dev.psxlab.core.ShaderChain
import kr.co.iefriends.pcsx2.NativeApp
import org.json.JSONObject
import org.junit.Assert.*
import org.junit.Test
import org.junit.runner.RunWith
import java.io.File

@RunWith(AndroidJUnit4::class)
class ShaderDefaultsIntegrationTest {
    @Test fun globalShaderRestoresForMultipleGamesAndChangedMarkersReactToReset() {
        assertEquals("true", InstrumentationRegistry.getArguments().getString("shaderDefaultsAudit"))
        val inst = InstrumentationRegistry.getInstrumentation()
        val context = inst.targetContext
        assertFalse("Run this check with no active game", NativeApp.hasActiveVM())
        val previousPrefs = runCatching { MainActivityRuntime.prefs }.getOrElse { context.getSharedPreferences("ARMSX2", Context.MODE_PRIVATE) }
        val field = MainActivityRuntime::class.java.getDeclaredField("lastInitDataRoot").apply { isAccessible = true }
        val previousRoot = field.get(null)
        val directory = File(context.cacheDir, "settings-change-audit").apply { mkdirs() }
        val preferences = context.getSharedPreferences("shader-defaults-audit", Context.MODE_PRIVATE)
        preferences.edit().clear().commit()
        MainActivityRuntime.prefs = preferences
        field.set(null, directory.absolutePath)
        var activity: SettingsChangeAuditActivity? = null
        try {
            val preset = File(directory, "psxlab-phases.slangp")
            for (name in listOf("psxlab-phases.slangp", "psxlab-phases.slang"))
                inst.context.assets.open(name).use { input -> File(directory, name).outputStream().use(input::copyTo) }
            ConfigStore.saveGlobal(Settings())
            ConfigStore.update(SettingsScope.Global, null) { it.copy(shaderChainEnabled = true, shaderChainPreset = preset.absolutePath) }
            ConfigStore.saveOverrides("GAME-A", JSONObject().put("affinityMode", 2))
            for (game in listOf("GAME-A", "GAME-B")) {
                ShaderChain.request(false, "")
                Ps1Shaders.pushForGame(game)
                assertTrue("Global shader was not queued for $game", ShaderChain.requested())
                assertTrue(ShaderChain.status(), ShaderChain.status().contains(preset.absolutePath))
            }
            val globalBefore = preferences.getString("config.global", null)
            ConfigStore.update(SettingsScope.Game, "GAME-A") { it.copy(shaderChainEnabled = false) }
            Ps1Shaders.pushForGame("GAME-A");assertFalse(ShaderChain.requested())
            Ps1Shaders.pushForGame("GAME-B");assertTrue(ShaderChain.requested())
            assertEquals(globalBefore, preferences.getString("config.global", null))

            // Allows the store/JNI regression to run without taking the display from another app.
            if (InstrumentationRegistry.getArguments().getString("headlessShaderDefaults") == "true") return

            activity = inst.startActivitySync(Intent(context, SettingsChangeAuditActivity::class.java)
                .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)) as SettingsChangeAuditActivity
            val screen = activity
            val readyAt = SystemClock.uptimeMillis() + 5000
            while (changedCount(inst.uiAutomation.rootInActiveWindow) < 2 && SystemClock.uptimeMillis() < readyAt) SystemClock.sleep(50)
            val initial = changedCount(inst.uiAutomation.rootInActiveWindow)
            assertTrue("Global shader toggle/preset should be marked", initial >= 2)
            inst.runOnMainSync { Ps1SettingsStore.update(screen.auditContext, null) { it.copy(fastBoot = true, internalScale = 3) } }
            awaitCount(initial + 2)
            File(context.getExternalFilesDir(null), "performance").apply { mkdirs() }.let { output ->
                inst.uiAutomation.takeScreenshot()?.let { bitmap ->
                    File(output, "settings-changed.png").outputStream().use { bitmap.compress(Bitmap.CompressFormat.PNG, 100, it) };bitmap.recycle()
                }
            }
            inst.runOnMainSync { Ps1SettingsStore.update(screen.auditContext, null) { Ps1Settings() } }
            awaitCount(initial)
            inst.runOnMainSync { ConfigStore.update(SettingsScope.Global, null) { it.copy(shaderChainEnabled = false, shaderChainPreset = "") } }
            awaitCount(0)
        } finally {
            activity?.let { screen -> inst.runOnMainSync(screen::finish) }
            MainActivityRuntime.prefs = previousPrefs
            field.set(null, previousRoot)
            Ps1Shaders.pushForGame(Ps1SettingsStore.activeGameKey)
            preferences.edit().clear().commit()
        }
    }
    private fun awaitCount(expected: Int) {
        val inst = InstrumentationRegistry.getInstrumentation()
        val deadline = SystemClock.uptimeMillis() + 5000
        var actual = -1
        while (SystemClock.uptimeMillis() < deadline) {
            inst.waitForIdleSync(); actual = changedCount(inst.uiAutomation.rootInActiveWindow)
            if (actual == expected) return
            SystemClock.sleep(60)
        }
        assertEquals("Changed markers after edit/reset", expected, actual)
    }
    private fun changedCount(node: AccessibilityNodeInfo?): Int {
        if (node == null) return 0
        var count = if (node.contentDescription?.toString() == "Changed marks a value that differs from its factory default.") 1 else 0
        for (index in 0 until node.childCount) count += changedCount(node.getChild(index))
        return count
    }
}
