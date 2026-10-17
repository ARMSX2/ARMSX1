// SPDX-License-Identifier: GPL-3.0-or-later
package com.armsx2.ui.manual

import androidx.activity.compose.BackHandler
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.WindowInsets
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.safeDrawing
import androidx.compose.foundation.layout.windowInsetsPadding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.selection.SelectionContainer
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.key
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.rememberUpdatedState
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.runtime.withFrameNanos
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.semantics.heading
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import com.armsx2.i18n.str
import com.armsx2.navigation.ManualReaderNavigation
import com.armsx2.ui.common.ArmsBackdrop
import com.armsx2.ui.common.ArmsTopBar
import com.armsx2.ui.common.GlassPanel
import com.armsx2.ui.common.RoundAction
import com.armsx2.ui.home.LibraryKeyboard
import com.armsx2.ui.settings.SettingsControllerNav
import com.armsx2.ui.settings.controllerFocusable

@Composable
fun ManualScreen(onBack: () -> Unit) {
    var chapterId by rememberSaveable { mutableStateOf<String?>(null) }
    var query by rememberSaveable { mutableStateOf("") }
    val chapter = MANUAL_CHAPTERS.find { it.id == chapterId }
    val searching = query.isNotBlank()
    val matches = if (searching) searchManual(query) else emptyList()
    val backInManual: () -> Boolean = {
        when {
            LibraryKeyboard.visible.value -> { LibraryKeyboard.close(); true }
            chapterId != null -> { chapterId = null; true }
            query.isNotEmpty() -> { query = ""; true }
            else -> false
        }
    }
    val latestBack by rememberUpdatedState(backInManual)
    DisposableEffect(Unit) {
        val release = SettingsControllerNav.claimLayer("manual")
        ManualReaderNavigation.handler = { latestBack() }
        onDispose {
            ManualReaderNavigation.handler = null
            LibraryKeyboard.close()
            release()
        }
    }
    BackHandler(enabled = chapterId != null || query.isNotEmpty() || LibraryKeyboard.visible.value) { backInManual() }
    LaunchedEffect(chapterId, query) {
        SettingsControllerNav.clearSelection()
        var attempts = 0
        while (!SettingsControllerNav.selectById("manual.search") && attempts++ < 10) withFrameNanos {}
    }
    val search = {
        LibraryKeyboard.open(query, { query = it; chapterId = null }, "Search the instruction manual…")
    }
    ArmsBackdrop {
        Column(Modifier.fillMaxSize()) {
            ArmsTopBar(
                title = str("manual.title"),
                subtitle = chapter?.title ?: if (searching) "${matches.size} topics found" else "Contents · Available offline",
                leading = { RoundAction("←", str("action.back"), { if (!backInManual()) onBack() }) },
                actions = {
                    if (chapter != null || searching) RoundAction("☰", "Manual contents", {
                        LibraryKeyboard.close(); query = ""; chapterId = null
                    })
                },
            )
            Row(Modifier.fillMaxWidth().padding(horizontal = 12.dp, vertical = 6.dp),
                verticalAlignment = Alignment.CenterVertically) {
                Surface(onClick = search, shape = RoundedCornerShape(18.dp),
                    color = MaterialTheme.colorScheme.surfaceVariant,
                    modifier = Modifier.weight(1f).controllerFocusable("manual.search", onConfirm = search, layer = "manual")) {
                    Text(if (query.isEmpty()) "⌕  Search settings and features…" else "⌕  $query",
                        Modifier.padding(16.dp), style = MaterialTheme.typography.bodyLarge)
                }
                if (query.isNotEmpty()) RoundAction("×", "Clear manual search", { query = "" })
            }
            // Compose just the current chapter (or results), so the full manual never becomes
            // one giant page. Non-lazy cards keep the controller registry complete within it.
            key(chapterId, query) {
                Column(Modifier.weight(1f).fillMaxWidth().verticalScroll(rememberScrollState())
                    .windowInsetsPadding(WindowInsets.safeDrawing)
                    .padding(horizontal = 12.dp, vertical = 8.dp),
                    verticalArrangement = Arrangement.spacedBy(12.dp)) {
                    when {
                        searching -> {
                            if (matches.isEmpty()) Text("No matching topics. Try a setting name such as BIOS, audio, cheats or memory card.",
                                style = MaterialTheme.typography.bodyLarge)
                            matches.forEach { match ->
                                ManualTopicCard(match.topic, "${match.chapter.id}.${match.chapter.topics.indexOf(match.topic)}", match.chapter.title)
                            }
                        }
                        chapter != null -> {
                            Text(chapter.summary, style = MaterialTheme.typography.bodyLarge,
                                color = MaterialTheme.colorScheme.onSurfaceVariant)
                            chapter.topics.forEachIndexed { index, topic -> ManualTopicCard(topic, "${chapter.id}.$index") }
                        }
                        else -> {
                            Text("Choose a section or search for a setting. Reading this manual does not change your settings or files.",
                                style = MaterialTheme.typography.bodyLarge)
                            MANUAL_CHAPTERS.forEachIndexed { index, section ->
                                val open = { chapterId = section.id }
                                Surface(onClick = open, shape = RoundedCornerShape(22.dp),
                                    color = MaterialTheme.colorScheme.surface,
                                    modifier = Modifier.fillMaxWidth().controllerFocusable("manual.chapter.${section.id}",
                                        onConfirm = open, layer = "manual")) {
                                    Column(Modifier.padding(18.dp), verticalArrangement = Arrangement.spacedBy(6.dp)) {
                                        Text("${index + 1}. ${section.title}", style = MaterialTheme.typography.titleMedium,
                                            fontWeight = FontWeight.Bold, modifier = Modifier.semantics { heading() })
                                        Text(section.summary, style = MaterialTheme.typography.bodyLarge,
                                            color = MaterialTheme.colorScheme.onSurfaceVariant)
                                        Text("${section.topics.size} topics", style = MaterialTheme.typography.labelMedium)
                                    }
                                }
                            }
                        }
                    }
                    Spacer(Modifier.height(12.dp))
                }
            }
        }
        LibraryKeyboard.Overlay(this)
    }
}

@Composable
private fun ManualTopicCard(topic: ManualTopic, id: String, chapterTitle: String? = null) {
    GlassPanel(Modifier.fillMaxWidth().controllerFocusable("manual.topic.$id", layer = "manual")) {
        SelectionContainer {
            Column(verticalArrangement = Arrangement.spacedBy(10.dp)) {
                if (chapterTitle != null) Text(chapterTitle, style = MaterialTheme.typography.labelMedium,
                    color = MaterialTheme.colorScheme.primary)
                Text(topic.title, style = MaterialTheme.typography.titleMedium, fontWeight = FontWeight.Bold,
                    modifier = Modifier.semantics { heading() })
                Text(topic.body, style = MaterialTheme.typography.bodyLarge)
            }
        }
    }
}
