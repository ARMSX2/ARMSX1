package com.armsx2.ui.emulation

import com.armsx2.ui.common.padFocusRing

import android.content.Context
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.gestures.detectDragGesturesAfterLongPress
import androidx.compose.foundation.gestures.scrollBy
import androidx.compose.foundation.ScrollState
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.CompositionLocalProvider
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.staticCompositionLocalOf
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Rect
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.layout.Layout
import androidx.compose.ui.layout.boundsInRoot
import androidx.compose.ui.layout.layoutId
import androidx.compose.ui.layout.onGloballyPositioned
import androidx.compose.ui.layout.positionInRoot
import androidx.compose.ui.unit.Constraints
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.constrainWidth
import androidx.compose.ui.unit.constrainHeight
import org.json.JSONArray
import org.json.JSONObject
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch

internal data class MenuMoveTarget(val group: String, val id: String)
internal val LocalMenuMoveTarget = staticCompositionLocalOf<MenuMoveTarget?> { null }
private val LocalMenuOrderGroup = staticCompositionLocalOf<String?> { null }
internal class MenuDragViewport(val scroll: ScrollState, val horizontal: Boolean) {
    var bounds: Rect? = null
}
internal val LocalMenuDragViewport = staticCompositionLocalOf<MenuDragViewport?> { null }

internal object QuickMenuOrder {
    private const val PREF = "ui.quickMenuOrder"
    private var context: Context? = null
    private var orders by mutableStateOf<Map<String, List<String>>>(emptyMap())
    private var original: Map<String, List<String>>? = null
    private var lastDragTarget: MenuMoveTarget? = null
    var moving by mutableStateOf<MenuMoveTarget?>(null)
        private set
    private val children = mutableMapOf<String, List<String>>()
    private val bounds = mutableMapOf<MenuMoveTarget, Rect>()
    private val origins = mutableMapOf<MenuMoveTarget, Offset>()
    private val tabs = EmulationMenuTab.entries.map { it.name }

    fun load(ctx: Context) {
        if (context != null) return
        context = ctx.applicationContext
        val json = runCatching {
            JSONObject(ctx.getSharedPreferences("ARMSX2", 0).getString(PREF, "{}").orEmpty())
        }.getOrDefault(JSONObject())
        orders = json.keys().asSequence().associateWith { group ->
            val array = json.optJSONArray(group) ?: JSONArray()
            (0 until array.length()).mapNotNull { array.optString(it).takeIf(String::isNotBlank) }.distinct()
        }
        orders["Session"]?.let { previous ->
            val updated = MenuOrderPolicy.insertAfterIfNew(previous, "action.discord", "action.screenshot")
            if (updated != previous) { orders = orders + ("Session" to updated); persist() }
        }
        children["tabs"] = tabs
    }
    fun tabOrder(): List<EmulationMenuTab> =
        MenuOrderPolicy.visible(orders["tabs"].orEmpty(), tabs).map { EmulationMenuTab.valueOf(it) }
    fun visible(group: String, defaults: List<String>): List<String> {
        children[group] = defaults
        return MenuOrderPolicy.visible(orders[group].orEmpty(), defaults)
    }
    fun position(target: MenuMoveTarget, rect: Rect, origin: Offset) { bounds[target] = rect; origins[target] = origin }
    fun itemOrigin(target: MenuMoveTarget): Offset = origins[target] ?: Offset.Zero
    fun unregister(target: MenuMoveTarget) { bounds.remove(target); origins.remove(target) }
    fun begin(target: MenuMoveTarget) {
        if (moving != null || target.id !in children[target.group].orEmpty()) return
        original = orders
        lastDragTarget = null
        moving = target
        com.armsx2.MenuSfx.play(com.armsx2.MenuSfx.Event.SELECT)
    }
    fun beginTouch(target: MenuMoveTarget, point: Offset): Boolean {
        // Nested rows get the gesture before their containing section. Hold its title to move the section.
        if (bounds.any { (child, rect) ->
            child.group.startsWith("${target.group}/${target.id}") && rect.contains(point)
        }) return false
        if (moving != null) return false
        begin(target)
        return moving == target
    }
    fun moveTo(target: String) {
        val item = moving ?: return
        val next = MenuOrderPolicy.move(orders[item.group].orEmpty(), children[item.group].orEmpty(), item.id, target)
        if (next != orders[item.group]) {
            orders = orders + (item.group to next)
            com.armsx2.MenuSfx.play(com.armsx2.MenuSfx.Event.NAV)
        }
    }
    fun step(delta: Int) {
        val item = moving ?: return
        val visible = MenuOrderPolicy.visible(orders[item.group].orEmpty(), children[item.group].orEmpty())
        visible.getOrNull(visible.indexOf(item.id) + delta)?.let(::moveTo)
    }
    fun drag(point: Offset) {
        val item = moving ?: return
        if (bounds[item]?.contains(point) == true) {
            lastDragTarget = null
            return
        }
        bounds.entries.firstOrNull { (other, rect) ->
            other.group == item.group && other.id != item.id && rect.contains(point)
        }?.let {
            // Multiple input events can arrive before layout catches up with a swap.
            // Do not immediately swap back against that same, still-old rectangle.
            if (lastDragTarget != it.key) { lastDragTarget = it.key; moveTo(it.key.id) }
        }
    }
    private fun persist() {
        val json = JSONObject()
        orders.forEach { (group, ids) -> json.put(group, JSONArray(ids)) }
        context?.getSharedPreferences("ARMSX2", 0)?.edit()?.putString(PREF, json.toString())?.apply()
    }
    fun finish(save: Boolean) {
        if (moving == null) return
        if (save) {
            persist()
        } else original?.let { orders = it }
        moving = null
        original = null
        lastDragTarget = null
    }
}

/** Reorder placement, not composition: remembered values, focus and capture callbacks stay intact. */
@Composable
internal fun MenuOrderColumn(
    group: String,
    modifier: Modifier = Modifier,
    spacing: Dp = 10.dp,
    content: @Composable () -> Unit,
) {
    CompositionLocalProvider(LocalMenuOrderGroup provides group) {
        Layout(content = content, modifier = modifier.fillMaxWidth()) { measurables, constraints ->
            val measured = measurables.map { it to it.measure(Constraints(maxWidth = constraints.maxWidth)) }
                .filter { (_, placeable) -> placeable.height > 0 }
            val ids = measured.mapNotNull { (item, _) -> item.layoutId as? String }
            val order = QuickMenuOrder.visible(group, ids)
            // Unkeyed explanatory text/spacers retain their slots; keyed options move between them.
            val keyed = measured.filter { it.first.layoutId is String }.sortedBy { order.indexOf(it.first.layoutId) }.iterator()
            val placeables = measured.map { if (it.first.layoutId is String) keyed.next().second else it.second }
            val gap = spacing.roundToPx()
            val height = placeables.sumOf { it.height } + gap * (placeables.size - 1).coerceAtLeast(0)
            layout(constraints.constrainWidth(placeables.maxOfOrNull { it.width } ?: 0), constraints.constrainHeight(height)) {
                var y = 0
                placeables.forEach { it.placeRelative(0, y); y += it.height + gap }
            }
        }
    }
}

@Composable
internal fun MenuOrderItem(id: String, group: String? = null, modifier: Modifier = Modifier.fillMaxWidth(), content: @Composable () -> Unit) {
    val parent = group ?: LocalMenuOrderGroup.current
    if (parent == null) { content(); return }
    val target = MenuMoveTarget(parent, id)
    val moving = QuickMenuOrder.moving
    val viewport = LocalMenuDragViewport.current
    val scope = rememberCoroutineScope()
    DisposableEffect(target) { onDispose { QuickMenuOrder.unregister(target) } }
    Box(modifier.layoutId(id)
        .onGloballyPositioned { QuickMenuOrder.position(target, it.boundsInRoot(), it.positionInRoot()) }
        .pointerInput(target, viewport) {
            var point = Offset.Zero
            var ownsDrag = false
            var edgeScroll: Job? = null
            fun stopDrag(save: Boolean) {
                edgeScroll?.cancel()
                edgeScroll = null
                if (ownsDrag) QuickMenuOrder.finish(save)
                ownsDrag = false
            }
            try {
                detectDragGesturesAfterLongPress(
                onDragStart = { offset ->
                    // Position is relative to this item; convert once, then track the finger in root space.
                    point = QuickMenuOrder.itemOrigin(target) + offset
                    ownsDrag = QuickMenuOrder.beginTouch(target, point)
                    if (ownsDrag && viewport != null) edgeScroll = scope.launch(Dispatchers.Main) {
                        while (true) {
                            viewport.bounds?.let { rect ->
                                val coordinate = if (viewport.horizontal) point.x else point.y
                                val start = if (viewport.horizontal) rect.left else rect.top
                                val end = if (viewport.horizontal) rect.right else rect.bottom
                                val edge = 40.dp.toPx()
                                val step = when {
                                    coordinate < start + edge -> -8.dp.toPx()
                                    coordinate > end - edge -> 8.dp.toPx()
                                    else -> 0f
                                }
                                if (step != 0f) { viewport.scroll.scrollBy(step); QuickMenuOrder.drag(point) }
                            }
                            delay(16)
                        }
                    }
                },
                onDrag = { change, amount ->
                    if (ownsDrag) { change.consume(); point += amount; QuickMenuOrder.drag(point) }
                },
                onDragEnd = { stopDrag(true) },
                onDragCancel = { stopDrag(false) },
                )
            } finally {
                stopDrag(false)
            }
        }
        .then(if (moving == target) Modifier.border(2.dp, Color(0xFFFFC93C), RoundedCornerShape(14.dp)) else Modifier)) {
        CompositionLocalProvider(LocalMenuMoveTarget provides target) { content() }
        // Move mode owns touch too: an accidental tap cannot change a setting or close the game.
        if (moving != null) Box(Modifier.matchParentSize().clickable {
            if (moving.group == target.group) QuickMenuOrder.moveTo(target.id)
        })
    }
}

@Composable
internal fun MenuMoveControls() {
    val moving = QuickMenuOrder.moving
    Column(Modifier.fillMaxWidth().padding(horizontal = 10.dp)) {
        Text(if (moving == null) "Left: options · Right: tabs · X: next/increase · Hold A or touch to move"
            else "Move with directions · Release, then A to save · B to cancel",
            style = MaterialTheme.typography.labelSmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
        if (moving != null) Row(horizontalArrangement = Arrangement.spacedBy(4.dp)) {
            TextButton(modifier = Modifier.padFocusRing(), onClick = { QuickMenuOrder.step(-1) }) { Text("↑ / ←") }
            TextButton(modifier = Modifier.padFocusRing(), onClick = { QuickMenuOrder.step(1) }) { Text("↓ / →") }
            TextButton(modifier = Modifier.padFocusRing(), onClick = { QuickMenuOrder.finish(true) }) { Text("Done") }
            TextButton(modifier = Modifier.padFocusRing(), onClick = { QuickMenuOrder.finish(false) }) { Text(com.armsx2.i18n.str("action.cancel")) }
        }
    }
}
