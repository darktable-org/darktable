/* SPDX-License-Identifier: GPL-3.0-or-later */
package org.example.darktableandroid

import androidx.compose.foundation.Canvas
import androidx.compose.foundation.background
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.rememberScrollState
import androidx.compose.material3.DropdownMenu
import androidx.compose.material3.DropdownMenuItem
import androidx.compose.material3.FilterChip
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.key
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.graphics.Path
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.unit.dp

/** Channel selection is presentation only; edits retain the original native channel number. */
@Composable
internal fun ParametricMaskEditor(
    controls: ParametricMaskControls,
    editable: Boolean,
    onParameter: (String, Double) -> Unit,
) {
    if(controls.isEmpty) return
    Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
        Text(stringResource(R.string.parametric_mask_title), style = MaterialTheme.typography.titleSmall)
        controls.global.forEach { parameter ->
            key(parameter.name) { ParameterControl(parameter, editable) { onParameter(parameter.name, it) } }
        }
        if(controls.channels.isNotEmpty()) {
            var showOutput by rememberSaveable { mutableStateOf(false) }
            val direction = if(controls.channels.any { it.output == showOutput }) showOutput else controls.channels.first().output
            val channels = controls.channels.filter { it.output == direction }
            var inputSelection by rememberSaveable { mutableIntStateOf(-1) }
            var outputSelection by rememberSaveable { mutableIntStateOf(-1) }
            val chosen = if(direction) outputSelection else inputSelection
            val active = channels.firstOrNull { it.index == chosen } ?: channels.first()
            Row(Modifier.horizontalScroll(rememberScrollState()), horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                listOf(false, true).forEach { output ->
                    if(controls.channels.any { it.output == output }) FilterChip(selected = direction == output,
                        onClick = { showOutput = output },
                        label = { Text(stringResource(if(output) R.string.parametric_output else R.string.parametric_input)) })
                }
            }
            var expanded by remember { mutableStateOf(false) }
            val channelDescription = stringResource(R.string.parametric_channel_selector)
            Box {
                OutlinedButton(shape = MaterialTheme.shapes.small, onClick = { expanded = true }, modifier = Modifier.semantics {
                    contentDescription = channelDescription
                }) { Text(active.label) }
                DropdownMenu(expanded = expanded, onDismissRequest = { expanded = false }) {
                    channels.forEach { channel ->
                        DropdownMenuItem(text = { Text(channel.label) }, onClick = {
                            if(direction) outputSelection = channel.index else inputSelection = channel.index
                            expanded = false
                        })
                    }
                }
            }
            key(active.index) {
                ParametricMaskGraph(active)
                Text(stringResource(R.string.parametric_threshold_hint), style = MaterialTheme.typography.bodySmall)
                active.controls.filter { it.type != "action" }.forEach { parameter ->
                    key(parameter.name) { ParameterControl(parameter, editable) { onParameter(parameter.name, it) } }
                }
                active.thresholds.indices.forEach { handle ->
                    active.boundedThreshold(handle)?.let { parameter ->
                        key(parameter.name) {
                            // At the original full-span defaults an outer handle is
                            // locked against its inner neighbor. Keep its slider
                            // visible until moving that neighbor opens the range.
                            if(parameter.minimum == parameter.maximum) ParameterSlider(parameter.label,
                                parameter.value, 0.0, 1.0, false, onValue = {})
                            ParameterControl(parameter, editable) { onParameter(parameter.name, it) }
                        }
                    }
                }
                active.controls.filter { it.type == "action" }.forEach { parameter ->
                    key(parameter.name) { ParameterControl(parameter, editable) { onParameter(parameter.name, it) } }
                }
            }
        }
    }
}

@Composable
private fun ParametricMaskGraph(channel: ParametricMaskChannel) {
    val stroke = MaterialTheme.colorScheme.primary
    val grid = MaterialTheme.colorScheme.outlineVariant
    val description = stringResource(R.string.parametric_graph_description, channel.label)
    val samples = remember(channel) {
        val positions = (0..128).map { it / 128.0 } + channel.thresholds.map { it.value }
        positions.distinct().sorted().mapNotNull { x -> parametricThresholdDiagramValue(channel, x)?.let { x to it } }
    }
    Canvas(Modifier.fillMaxWidth().height(112.dp).background(MaterialTheme.colorScheme.surfaceVariant)
        .semantics { contentDescription = description }) {
        val padding = 12.dp.toPx()
        val width = (size.width - padding * 2).coerceAtLeast(1f)
        val height = (size.height - padding * 2).coerceAtLeast(1f)
        fun point(x: Double, y: Double) = Offset(padding + x.toFloat() * width, padding + (1f - y.toFloat()) * height)
        drawLine(grid, point(0.0, 0.0), point(1.0, 0.0))
        drawLine(grid, point(0.0, 1.0), point(1.0, 1.0))
        val path = Path()
        samples.forEachIndexed { index, value ->
            val position = point(value.first, value.second)
            if(index == 0) path.moveTo(position.x, position.y) else path.lineTo(position.x, position.y)
        }
        drawPath(path, stroke, style = Stroke(2.dp.toPx()))
        channel.thresholds.forEachIndexed { handle, threshold ->
            val y = if(handle == 0 || handle == 3) 0.0 else 1.0
            val position = point(threshold.value, if(channel.inverted) 1.0 - y else y)
            drawLine(grid, point(threshold.value, 0.0), point(threshold.value, 1.0))
            drawCircle(stroke, 3.dp.toPx(), position)
        }
    }
}
