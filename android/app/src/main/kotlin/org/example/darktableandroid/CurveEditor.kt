/* SPDX-License-Identifier: GPL-3.0-or-later */
package org.example.darktableandroid

import androidx.compose.foundation.Canvas
import androidx.compose.foundation.background
import androidx.compose.foundation.gestures.awaitEachGesture
import androidx.compose.foundation.gestures.awaitFirstDown
import androidx.compose.foundation.gestures.drag
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.runtime.saveable.listSaver
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.graphics.Path
import androidx.compose.ui.graphics.PathEffect
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.graphics.drawscope.clipRect
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.unit.dp
import org.example.darktableandroid.nativecore.*

private val CurveNodesSaver = listSaver<List<CurvePoint>, Float>(
    save = { nodes -> nodes.flatMap { listOf(it.x, it.y) } },
    restore = { values -> values.chunked(2).map { CurvePoint(it[0], it[1]) } },
)

@Composable
internal fun ModuleCurves(
    curves: List<ModuleCurve>, editable: Boolean,
    onApply: (Int, Int, List<CurvePoint>) -> Unit,
) {
    var channel by rememberSaveable { mutableIntStateOf(curves.first().channel) }
    val active = curves.firstOrNull { it.channel == channel } ?: curves.first()
    if(curves.size > 1) ScrollableTabRow(selectedTabIndex = curves.indexOf(active), edgePadding = 0.dp) {
        curves.forEach { curve ->
            Tab(selected = curve.channel == active.channel, onClick = { channel = curve.channel },
                text = { Text(curve.label) })
        }
    }
    // Keep each draft composed while switching channels. A channel switch
    // does not silently discard edits that have not yet been applied.
    curves.forEach { curve ->
        key(curve.channel) {
            CurveEditor(curve, editable && curve.editable, curve.channel == active.channel) { interpolation, nodes ->
                onApply(curve.channel, interpolation, nodes)
            }
        }
    }
}

@Composable
private fun CurveEditor(
    curve: ModuleCurve, editable: Boolean, visible: Boolean,
    onApply: (Int, List<CurvePoint>) -> Unit,
) {
    // A tool can be absent while Undo changes its native snapshot. Include
    // that snapshot in the saved draft's identity, while retaining point and
    // channel selection outside it.
    var nodes by key(curve.nodes, curve.interpolation) {
        rememberSaveable(stateSaver = CurveNodesSaver) { mutableStateOf(curve.nodes) }
    }
    var interpolation by key(curve.nodes, curve.interpolation) {
        rememberSaveable { mutableIntStateOf(curve.interpolation) }
    }
    var selection by rememberSaveable { mutableIntStateOf(0) }
    if(!visible) return
    val selected = selection.coerceIn(0, nodes.lastIndex)
    val point = nodes[selected]
    var xText by remember(selected, point) { mutableStateOf(point.x.toString()) }
    var yText by remember(selected, point) { mutableStateOf(point.y.toString()) }
    val numericNodes = xText.toFloatOrNull()?.let { x -> yText.toFloatOrNull()?.let { y ->
        replaceCurvePoint(nodes, selected, x, y, curve.maxNodes, curve.minSpacing)
    } }
    val changed = nodes != curve.nodes || interpolation != curve.interpolation
    var pointsMenu by remember { mutableStateOf(false) }
    var interpolationMenu by remember { mutableStateOf(false) }
    val interpolationNames = listOf(stringResource(R.string.curve_cubic), stringResource(R.string.curve_catmull),
        stringResource(R.string.curve_monotone))

    Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
        Text(curve.label, style = MaterialTheme.typography.titleSmall)
        CurveGraph(curve, nodes, selected, editable, changed,
            onSelect = { selection = it }, onMove = { index, x, y ->
                moveCurvePoint(nodes, index, x, y, curve.maxNodes, curve.minSpacing)?.let { nodes = it }
            })
        Text(stringResource(if(changed) R.string.curve_pending else R.string.curve_applied),
            style = MaterialTheme.typography.bodySmall)
        if(!curve.editable) Text(stringResource(R.string.curve_read_only), style = MaterialTheme.typography.bodySmall)
        Box {
            OutlinedButton(onClick = { interpolationMenu = true }, enabled = editable) {
                Text(interpolationNames[interpolation])
            }
            DropdownMenu(expanded = interpolationMenu, onDismissRequest = { interpolationMenu = false }) {
                interpolationNames.forEachIndexed { index, name ->
                    DropdownMenuItem(text = { Text(name) }, onClick = { interpolation = index; interpolationMenu = false })
                }
            }
        }
        Box {
            TextButton(onClick = { pointsMenu = true }) { Text(stringResource(R.string.curve_point, selected + 1, nodes.size)) }
            DropdownMenu(expanded = pointsMenu, onDismissRequest = { pointsMenu = false }) {
                nodes.forEachIndexed { index, _ ->
                    DropdownMenuItem(text = { Text(stringResource(R.string.curve_point, index + 1, nodes.size)) },
                        onClick = { selection = index; pointsMenu = false })
                }
            }
        }
        Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            OutlinedTextField(value = xText, onValueChange = { xText = it }, modifier = Modifier.weight(1f),
                label = { Text(stringResource(R.string.curve_input)) }, enabled = editable,
                singleLine = true, isError = numericNodes == null,
                keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Ascii))
            OutlinedTextField(value = yText, onValueChange = { yText = it }, modifier = Modifier.weight(1f),
                label = { Text(stringResource(R.string.curve_output)) }, enabled = editable,
                singleLine = true, isError = numericNodes == null,
                keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Ascii))
        }
        Text(stringResource(R.string.curve_coordinate_range), style = MaterialTheme.typography.bodySmall)
        TextButton(onClick = { numericNodes?.let { nodes = it } },
            enabled = editable && numericNodes != null && numericNodes != nodes) { Text(stringResource(R.string.curve_update_point)) }
        Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            TextButton(onClick = {
                addCurvePoint(nodes, curve.maxNodes, curve.minSpacing)?.let { nodes = it.nodes; selection = it.index }
            }, enabled = editable && addCurvePoint(nodes, curve.maxNodes, curve.minSpacing) != null) {
                Text(stringResource(R.string.curve_add))
            }
            TextButton(onClick = {
                deleteCurvePoint(nodes, selected, curve.maxNodes, curve.minSpacing)?.let {
                    nodes = it; selection = selected.coerceAtMost(it.lastIndex)
                }
            }, enabled = editable && nodes.size > 2) { Text(stringResource(R.string.curve_delete)) }
            TextButton(onClick = {
                nodes = curve.resetNodes ?: listOf(CurvePoint(0f, 0f), CurvePoint(1f, 1f)); selection = 0
                xText = nodes.first().x.toString(); yText = nodes.first().y.toString()
            },
                enabled = editable) { Text(stringResource(R.string.curve_reset)) }
        }
        Button(onClick = { onApply(interpolation, nodes.toList()) }, enabled = editable && changed &&
            numericNodes == nodes && validCurveNodes(nodes, curve.maxNodes, curve.minSpacing)) {
            Text(stringResource(R.string.curve_apply))
        }
    }
}

@Composable
private fun CurveGraph(
    curve: ModuleCurve, nodes: List<CurvePoint>, selected: Int, editable: Boolean, changed: Boolean,
    onSelect: (Int) -> Unit, onMove: (Int, Float, Float) -> Unit,
) {
    val density = LocalDensity.current
    val padding = with(density) { 16.dp.toPx() }
    val hitRadius = with(density) { 28.dp.toPx() }
    val currentNodes by rememberUpdatedState(nodes)
    val select by rememberUpdatedState(onSelect)
    val move by rememberUpdatedState(onMove)
    val appliedColor = MaterialTheme.colorScheme.primary
    val draftColor = MaterialTheme.colorScheme.tertiary
    val gridColor = MaterialTheme.colorScheme.outlineVariant
    val pointColor = MaterialTheme.colorScheme.onSurface
    val description = stringResource(R.string.curve_graph_description, curve.label)
    Canvas(Modifier.fillMaxWidth().height(240.dp).background(MaterialTheme.colorScheme.surfaceVariant)
        .semantics { contentDescription = description }
        .pointerInput(editable, padding, hitRadius) {
            awaitEachGesture {
                val down = awaitFirstDown()
                if(!editable) return@awaitEachGesture
                val width = (size.width - padding * 2).coerceAtLeast(1f)
                val height = (size.height - padding * 2).coerceAtLeast(1f)
                fun location(point: CurvePoint) = Offset(padding + point.x * width, padding + (1f - point.y) * height)
                val nearest = currentNodes.indices.minByOrNull { (location(currentNodes[it]) - down.position).getDistanceSquared() }
                    ?.takeIf { (location(currentNodes[it]) - down.position).getDistance() <= hitRadius }
                    ?: return@awaitEachGesture
                select(nearest)
                down.consume()
                drag(down.id) { change ->
                    move(nearest, (change.position.x - padding) / width, 1f - (change.position.y - padding) / height)
                    change.consume()
                }
            }
        }) {
        val width = (size.width - padding * 2).coerceAtLeast(1f)
        val height = (size.height - padding * 2).coerceAtLeast(1f)
        fun location(point: CurvePoint) = Offset(padding + point.x * width, padding + (1f - point.y) * height)
        for(step in 0..4) {
            val fraction = step / 4f
            drawLine(gridColor, Offset(padding + fraction * width, padding), Offset(padding + fraction * width, padding + height))
            drawLine(gridColor, Offset(padding, padding + fraction * height), Offset(padding + width, padding + fraction * height))
        }
        clipRect(padding, padding, padding + width, padding + height) {
            // These samples come directly from the upstream native curve
            // implementation. Android only draws their polyline.
            val applied = Path()
            curve.samples.forEachIndexed { index, sample ->
                val position = location(sample)
                if(index == 0) applied.moveTo(position.x, position.y) else applied.lineTo(position.x, position.y)
            }
            drawPath(applied, appliedColor, style = Stroke(2.dp.toPx()))
            if(changed) {
                val controlPolygon = Path()
                nodes.forEachIndexed { index, node ->
                    val position = location(node)
                    if(index == 0) controlPolygon.moveTo(position.x, position.y) else controlPolygon.lineTo(position.x, position.y)
                }
                drawPath(controlPolygon, draftColor,
                    style = Stroke(1.dp.toPx(), pathEffect = PathEffect.dashPathEffect(floatArrayOf(6.dp.toPx(), 4.dp.toPx()))))
            }
        }
        nodes.forEachIndexed { index, node ->
            drawCircle(if(index == selected) draftColor else pointColor, if(index == selected) 6.dp.toPx() else 4.dp.toPx(), location(node))
        }
    }
}
