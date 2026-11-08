/* SPDX-License-Identifier: GPL-3.0-or-later */
package org.example.darktableandroid

import android.graphics.Paint
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.material3.DropdownMenu
import androidx.compose.material3.DropdownMenuItem
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.key
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Path
import androidx.compose.ui.graphics.PathEffect
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.graphics.drawscope.clipRect
import androidx.compose.ui.graphics.drawscope.rotate
import androidx.compose.ui.graphics.nativeCanvas
import androidx.compose.ui.graphics.toArgb
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import org.example.darktableandroid.nativecore.ModuleParameter
import kotlin.math.cos
import kotlin.math.sin

/** Select forms by native identity and nodes by the current applied topology. */
@Composable
internal fun MaskControls(
    parameters: List<ModuleParameter>, editable: Boolean, visibleNames: Set<String>? = null,
    onParameter: (String, Double) -> Unit,
) {
    fun visible(parameter: ModuleParameter) = visibleNames == null || parameter.name in visibleNames
    val groups = parameters.mapNotNull { parameter ->
        maskControlAddress(parameter.name)?.let { it.formId to parameter }
    }.groupBy({ it.first }, { it.second })
    val candidates = groups.filterValues { fields -> fields.any(::visible) }
    var selectedId by rememberSaveable { mutableStateOf<Int?>(null) }
    val selected = selectedId?.takeIf { it in candidates } ?: candidates.keys.firstOrNull()
    Column(verticalArrangement = Arrangement.spacedBy(6.dp)) {
        parameters.filter { maskControlAddress(it.name) == null && visible(it) }.forEach { parameter ->
            key(parameter.name) { ParameterControl(parameter, editable) { onParameter(parameter.name, it) } }
        }
        if(selected != null) {
            MaskChoice(candidates.keys.map { id -> id to maskControlLabel(id, groups.getValue(id)) }, selected) { selectedId = it }
            val fields = groups.getValue(selected)
            val shape = maskDiagramShapes(fields).singleOrNull()
            val nodeIndexes = fields.filter(::visible).mapNotNull { maskControlAddress(it.name)?.nodeIndex }.distinct().sorted()
            var selectedNode by rememberSaveable(selected) { mutableStateOf(0) }
            val nodeIndex = selectedNode.takeIf { it in nodeIndexes } ?: nodeIndexes.firstOrNull()
            if(shape != null) MaskDiagramCanvas(shape, nodeIndex)
            if(nodeIndex != null) {
                MaskChoice(nodeIndexes.map { it to stringResource(R.string.mask_path_node, it + 1) }, nodeIndex,
                    description = stringResource(R.string.mask_path_select_node)) { selectedNode = it }
            }
            // Native insertion/deletion compacts node indexes; discard every old draft
            // after applied fields change, even if a new node reused the same index.
            fields.filter { parameter -> visible(parameter) &&
                (maskControlAddress(parameter.name)?.nodeIndex?.let { it == nodeIndex } ?: true)
            }.forEach { parameter ->
                key(parameter.name, fields) { ParameterControl(parameter, editable) { onParameter(parameter.name, it) } }
            }
        }
    }
}

@Composable
private fun MaskChoice(items: List<Pair<Int, String>>, selected: Int, description: String? = null, onSelected: (Int) -> Unit) {
    if(items.isEmpty()) return
    val label = items.firstOrNull { it.first == selected }?.second ?: items.first().second
    var expanded by rememberSaveable { mutableStateOf(false) }
    if(items.size == 1) Text(label, style = MaterialTheme.typography.labelLarge) else Box {
        OutlinedButton(shape = MaterialTheme.shapes.small, onClick = { expanded = true }, modifier = Modifier.semantics { if(description != null) contentDescription = description }) {
            Text(label)
        }
        DropdownMenu(expanded = expanded, onDismissRequest = { expanded = false }) {
            items.forEach { (id, title) ->
                DropdownMenuItem(text = { Text(title) }, onClick = { onSelected(id); expanded = false })
            }
        }
    }
}

/** A square coordinate schematic, separate from the processed photo and its distortions. */
@Composable
internal fun MaskDiagram(parameters: List<ModuleParameter>) {
    val shapes = maskDiagramShapes(parameters)
    if(shapes.isEmpty()) return
    var selectedId by rememberSaveable { mutableStateOf(shapes.first().id) }
    val shape = shapes.firstOrNull { it.id == selectedId } ?: shapes.first()
    Column(verticalArrangement = Arrangement.spacedBy(6.dp)) {
        MaskChoice(shapes.map { it.id to it.label }, shape.id) { selectedId = it }
        MaskDiagramCanvas(shape, shape.nodes.firstOrNull()?.index)
    }
}

@Composable
private fun MaskDiagramCanvas(shape: MaskDiagramShape, nodeIndex: Int?) {
    val gridColor = MaterialTheme.colorScheme.outlineVariant
    val shapeColor = MaterialTheme.colorScheme.primary.copy(alpha = if(shape.enabled) 1f else 0.4f)
    val sourceColor = MaterialTheme.colorScheme.tertiary
    val selectedNode = shape.nodes.firstOrNull { it.index == nodeIndex } ?: shape.nodes.firstOrNull()
    var description = if(selectedNode != null)
        stringResource(R.string.mask_path_diagram_description, shape.label, shape.nodes.size, selectedNode.index + 1,
            selectedNode.x.toString(), selectedNode.y.toString())
    else stringResource(R.string.mask_diagram_description, shape.label, shape.x.toString(), shape.y.toString())
    if(shape.sourceX != null && shape.sourceY != null)
        description += " " + stringResource(R.string.mask_diagram_source, shape.sourceX.toString(), shape.sourceY.toString())
    Canvas(Modifier.fillMaxWidth().height(200.dp).background(MaterialTheme.colorScheme.surfaceVariant)
        .semantics { contentDescription = description }) {
        val padding = 16.dp.toPx()
        val extent = (minOf(size.width, size.height) - padding * 2).coerceAtLeast(1f)
        val left = (size.width - extent) / 2f
        val top = (size.height - extent) / 2f
        fun position(x: Float, y: Float) = Offset(left + x * extent, top + y * extent)
        for(step in 0..4) {
            val fraction = step / 4f
            drawLine(gridColor, position(fraction, 0f), position(fraction, 1f))
            drawLine(gridColor, position(0f, fraction), position(1f, fraction))
        }
        val center = position(shape.x, shape.y)
        val line = 2.dp.toPx()
        val marker = 5.dp.toPx()
        val dashed = PathEffect.dashPathEffect(floatArrayOf(5.dp.toPx(), 4.dp.toPx()))
        val paint = Paint(Paint.ANTI_ALIAS_FLAG).apply { color = shapeColor.toArgb(); textSize = 11.sp.toPx() }
        clipRect(left, top, left + extent, top + extent) {
            when(shape.kind) {
                MaskDiagramKind.CIRCLE -> drawCircle(shapeColor, shape.radiusX * extent, center, style = Stroke(line))
                MaskDiagramKind.ELLIPSE -> rotate(shape.rotation, center) {
                    drawOval(shapeColor, Offset(center.x - shape.radiusX * extent, center.y - shape.radiusY * extent),
                        Size(shape.radiusX * extent * 2f, shape.radiusY * extent * 2f), style = Stroke(line))
                }
                MaskDiagramKind.GRADIENT -> {
                    val angle = Math.toRadians(-shape.rotation.toDouble())
                    val direction = Offset(cos(angle).toFloat(), sin(angle).toFloat()) * (extent * 0.4f)
                    drawLine(shapeColor, center - direction, center + direction, line)
                }
                MaskDiagramKind.PATH, MaskDiagramKind.BRUSH -> {
                    val path = Path().apply {
                        moveTo(center.x, center.y)
                        maskDiagramSegments(shape).forEach { segment ->
                            val outgoing = position(segment.start.control2X, segment.start.control2Y)
                            val incoming = position(segment.end.control1X, segment.end.control1Y)
                            val end = position(segment.end.x, segment.end.y)
                            cubicTo(outgoing.x, outgoing.y, incoming.x, incoming.y, end.x, end.y)
                        }
                        if(shape.kind == MaskDiagramKind.PATH) close()
                    }
                    if(shape.kind == MaskDiagramKind.PATH) drawPath(path, shapeColor.copy(alpha = 0.1f))
                    drawPath(path, shapeColor, style = Stroke(line))
                    shape.nodes.forEach { node ->
                        val anchor = position(node.x, node.y)
                        drawCircle(shapeColor, if(node.index == selectedNode?.index) marker else marker * 0.6f, anchor)
                        drawContext.canvas.nativeCanvas.drawText((node.index + 1).toString(), anchor.x + marker,
                            anchor.y - marker, paint)
                    }
                    selectedNode?.let { node ->
                        val anchor = position(node.x, node.y)
                        val handle1 = position(node.control1X, node.control1Y)
                        val handle2 = position(node.control2X, node.control2Y)
                        drawLine(sourceColor, anchor, handle1, line, pathEffect = dashed)
                        drawLine(sourceColor, anchor, handle2, line, pathEffect = dashed)
                        drawCircle(sourceColor, marker, handle1, style = Stroke(line))
                        drawCircle(sourceColor, marker, handle2, style = Stroke(line))
                        if(shape.kind == MaskDiagramKind.BRUSH) {
                            drawCircle(shapeColor.copy(alpha = 0.5f), node.borderIn * extent, anchor, style = Stroke(line))
                            drawCircle(shapeColor.copy(alpha = 0.5f), node.borderOut * extent, anchor, style = Stroke(line, pathEffect = dashed))
                        }
                    }
                }
            }
            if(shape.sourceX != null && shape.sourceY != null) {
                val source = position(shape.sourceX, shape.sourceY)
                drawLine(sourceColor, source, center, 1.dp.toPx(), pathEffect = dashed)
                drawCircle(sourceColor, marker, source, style = Stroke(line))
                drawLine(sourceColor, source - Offset(marker, 0f), source + Offset(marker, 0f), line)
                drawLine(sourceColor, source - Offset(0f, marker), source + Offset(0f, marker), line)
            }
            if(shape.nodes.isEmpty()) {
                drawCircle(shapeColor, 2.5.dp.toPx(), center)
                drawContext.canvas.nativeCanvas.drawText(shape.id.toString(), center.x + marker, center.y - marker, paint)
            }
        }
    }
    Text(stringResource(if(shape.nodes.isEmpty()) R.string.mask_diagram_hint else R.string.mask_path_diagram_hint),
        style = MaterialTheme.typography.bodySmall)
}
