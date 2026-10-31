// SPDX-License-Identifier: GPL-3.0-or-later
package com.armsx2.ui.common

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.FlowRow
import androidx.compose.foundation.layout.ExperimentalLayoutApi
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Surface
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.focus.FocusRequester
import androidx.compose.ui.focus.focusRequester
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalSoftwareKeyboardController
import androidx.compose.ui.unit.dp
import com.armsx2.DriverDebugPreferences
import com.armsx2.config.DriverDebugVariables
import com.armsx2.i18n.str
import com.armsx2.runtime.MainActivityRuntime
import com.armsx2.ui.settings.controllerFocusable
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

/** Process-wide options, deliberately outside the per-game settings inheritance. */
@OptIn(ExperimentalLayoutApi::class)
@Composable
internal fun DriverDebugOptionsSection() {
    val context = LocalContext.current
    val scope = rememberCoroutineScope()
    var saved by remember { mutableStateOf(DriverDebugPreferences.read(context)) }
    val running = remember { DriverDebugPreferences.forProcess(context) }
    var variables by rememberSaveable { mutableStateOf(saved.variables) }
    var ubwc by rememberSaveable { mutableStateOf(saved.ubwcHint) }
    var busy by remember { mutableStateOf(false) }
    var confirming by remember { mutableStateOf(false) }
    var message by remember { mutableStateOf<String?>(null) }
    val keyboard = LocalSoftwareKeyboardController.current
    val fieldFocus = remember { FocusRequester() }
    val draft = DriverDebugPreferences.Options(variables, ubwc)
    val validation = remember(variables, ubwc) { DriverDebugVariables.validate(variables, ubwc) }
    val saveFailed = str("backend.driver.debug.saveFailed")
    val savedMessage = str("backend.driver.debug.saved")
    val closeGame = str("backend.driver.debug.closeGame")

    fun persist(restart: Boolean) {
        if (busy || !validation.valid) return
        busy = true
        keyboard?.hide()
        scope.launch {
            val success = withContext(Dispatchers.IO) { DriverDebugPreferences.save(context, draft) }
            busy = false
            confirming = false
            if (!success) message = saveFailed
            else {
                saved = draft
                message = savedMessage
                if (restart && !MainActivityRuntime.restartAppIfIdle(context)) message = closeGame
            }
        }
    }

    Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
        SectionTitle(str("backend.driver.debug.title"), str("backend.driver.debug.description"))
        Surface(shape = RoundedCornerShape(16.dp), color = MaterialTheme.colorScheme.surfaceVariant) {
            Row(Modifier.fillMaxWidth().padding(14.dp).controllerFocusable(
                "driver.debug.ubwc", onConfirm = { if (!busy) { ubwc = !ubwc; confirming = false } },
            ), verticalAlignment = Alignment.CenterVertically) {
                Column(Modifier.weight(1f)) {
                    Text(str("backend.driver.debug.ubwc"), style = MaterialTheme.typography.titleSmall)
                    Text(str("backend.driver.debug.ubwcDescription"), style = MaterialTheme.typography.bodySmall)
                }
                Switch(checked = ubwc, enabled = !busy, onCheckedChange = { ubwc = it; confirming = false })
            }
        }
        OutlinedTextField(
            value = variables,
            onValueChange = { variables = it; confirming = false; message = null },
            label = { Text(str("backend.driver.debug.variables")) },
            placeholder = { Text("NAME=value") },
            // Do not put controllerFocusable on an editable field: it consumes Enter
            // (needed for new lines) and can focus its wrapper instead of the editor.
            modifier = Modifier.fillMaxWidth().focusRequester(fieldFocus),
            enabled = !busy,
            minLines = 3,
            maxLines = 8,
            isError = !validation.valid,
            supportingText = { validation.error?.let { Text(it) } },
        )
        if (saved != running) Text(str("backend.driver.debug.pending"), color = MaterialTheme.colorScheme.primary)
        message?.let { Text(it, style = MaterialTheme.typography.bodySmall) }
        FlowRow(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            OutlinedButton(
                enabled = !busy,
                onClick = { fieldFocus.requestFocus(); keyboard?.show() },
                modifier = Modifier.controllerFocusable("driver.debug.edit", onConfirm = {
                    if (!busy) { fieldFocus.requestFocus(); keyboard?.show() }
                }),
            ) { Text(str("backend.driver.debug.edit")) }
            OutlinedButton(
                enabled = !busy && validation.valid && draft != saved,
                onClick = { persist(false) },
                modifier = Modifier.controllerFocusable("driver.debug.save", onConfirm = { if (draft != saved) persist(false) }),
            ) { Text(str("action.save")) }
            OutlinedButton(
                enabled = !busy,
                onClick = { variables = ""; ubwc = false; confirming = false; message = null },
                modifier = Modifier.controllerFocusable("driver.debug.reset", onConfirm = {
                    if (!busy) { variables = ""; ubwc = false; confirming = false; message = null }
                }),
            ) { Text(str("backend.driver.debug.reset")) }
            OutlinedButton(
                enabled = !busy && validation.valid,
                onClick = { confirming = true },
                modifier = Modifier.controllerFocusable("driver.debug.apply", onConfirm = { if (!busy && validation.valid) confirming = true }),
            ) { Text(str("backend.driver.debug.apply")) }
        }
        // Inline confirmation stays in the same controller-navigation layer in both screens.
        // The native/VM guard runs again on confirmation; no active game is cold-killed.
        if (confirming) Surface(shape = RoundedCornerShape(16.dp), color = MaterialTheme.colorScheme.secondaryContainer) {
            Column(Modifier.padding(14.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                Text(str("backend.driver.debug.confirm"))
                FlowRow(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    OutlinedButton(enabled = !busy, onClick = { confirming = false }, modifier = Modifier.controllerFocusable(
                        "driver.debug.cancel", onConfirm = { if (!busy) confirming = false },
                    )) { Text(str("action.cancel")) }
                    OutlinedButton(enabled = !busy && validation.valid, onClick = { persist(true) }, modifier = Modifier.controllerFocusable(
                        "driver.debug.restart", onConfirm = { persist(true) },
                    )) { Text(str("backend.driver.debug.apply")) }
                }
            }
        }
    }
}
