package com.armsx2

import android.content.Context
import android.content.ContextWrapper
import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.unit.dp
import com.armsx2.config.*
import com.armsx2.i18n.str
import com.armsx2.ui.settings.*
import java.io.File

/** Internal UI fixture. Uses an isolated files directory supplied by the instrumentation test. */
class SettingsChangeAuditActivity : ComponentActivity() {
    lateinit var auditContext: Context
        private set
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        val directory = File(cacheDir, "settings-change-audit").apply { mkdirs() }
        auditContext = object : ContextWrapper(this) {
            override fun getFilesDir(): File = directory
            override fun getApplicationContext(): Context = this
        }
        setContent {
            CompositionLocalProvider(LocalContext provides auditContext) {
                MaterialTheme(colorScheme = darkColorScheme(primary = com.armsx2.ui.common.SelectionBlue)) {
                    ChangedSettingsProvider(null) {
                        val revision = Ps1SettingsStore.version.intValue
                        val settings = remember(revision) { Ps1SettingsStore.load(auditContext) }
                        Column(Modifier.fillMaxSize().background(MaterialTheme.colorScheme.background)
                            .verticalScroll(rememberScrollState()).padding(24.dp)) {
                            Text("Global settings", style = MaterialTheme.typography.headlineSmall,
                                color = MaterialTheme.colorScheme.onSurface)
                            Text(str("settings.changedFromDefault.help"), color = MaterialTheme.colorScheme.onSurfaceVariant)
                            ToggleRow(str("perf.skipBios.label"), settings.fastBoot) { value ->
                                Ps1SettingsStore.update(auditContext, null) { it.copy(fastBoot = value) }
                            }
                            SegmentedRow(str("renderer.internalRes.label"), listOf("Native", "3× HD"),
                                if (settings.internalScale == 1) 0 else 1) { index ->
                                Ps1SettingsStore.update(auditContext, null) { it.copy(internalScale = if (index == 0) 1 else 3) }
                            }
                            ScopedShaderSettings(null)
                        }
                    }
                }
            }
        }
    }
}
