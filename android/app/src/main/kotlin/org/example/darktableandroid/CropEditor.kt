/* SPDX-License-Identifier: GPL-3.0-or-later */
package org.example.darktableandroid

import androidx.compose.foundation.Canvas
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.PathEffect
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.unit.dp
import org.example.darktableandroid.nativecore.*

@Composable
internal fun CropEditor(crop: ModuleCrop, editable: Boolean, onApply: (CropRect) -> Unit) {
    var left by rememberSaveable(crop) { mutableStateOf(crop.bounds.left.toString()) }
    var top by rememberSaveable(crop) { mutableStateOf(crop.bounds.top.toString()) }
    var right by rememberSaveable(crop) { mutableStateOf(crop.bounds.right.toString()) }
    var bottom by rememberSaveable(crop) { mutableStateOf(crop.bounds.bottom.toString()) }
    var selectedAspect by rememberSaveable(crop) { mutableStateOf("free") }
    var portrait by rememberSaveable(crop) { mutableStateOf(crop.inputHeight > crop.inputWidth) }
    var referenceLeft by rememberSaveable(crop) { mutableFloatStateOf(crop.bounds.left) }
    var referenceTop by rememberSaveable(crop) { mutableFloatStateOf(crop.bounds.top) }
    var referenceRight by rememberSaveable(crop) { mutableFloatStateOf(crop.bounds.right) }
    var referenceBottom by rememberSaveable(crop) { mutableFloatStateOf(crop.bounds.bottom) }
    var ratioError by remember(crop) { mutableStateOf(false) }
    val draft = parseCropInput(left, top, right, bottom, crop.minSize)
    val changed = draft != crop.bounds || !crop.freehand
    fun manualEdit(update: () -> Unit) {
        update()
        selectedAspect = "free"
        ratioError = false
    }
    fun selectAspect(id: String, newPortrait: Boolean) {
        if(id == "free") {
            selectedAspect = id
            ratioError = false
            return
        }
        val reference = if(selectedAspect == "free") draft ?: crop.bounds else
            CropRect(referenceLeft, referenceTop, referenceRight, referenceBottom)
        val aspect = cropPresetAspect(id, crop.inputWidth, crop.inputHeight, newPortrait)
        val fitted = aspect?.let { fitCropAspect(reference, crop.inputWidth, crop.inputHeight, it, crop.minSize) }
        ratioError = fitted == null
        if(fitted != null) {
            referenceLeft = reference.left; referenceTop = reference.top
            referenceRight = reference.right; referenceBottom = reference.bottom
            left = fitted.left.toString(); top = fitted.top.toString()
            right = fitted.right.toString(); bottom = fitted.bottom.toString()
            selectedAspect = id
            portrait = newPortrait
        }
    }

    Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
        Text(stringResource(R.string.crop_bounds_title), style = MaterialTheme.typography.titleSmall)
        CropDiagram(crop.bounds, draft, crop.inputWidth, crop.inputHeight)
        Text(stringResource(R.string.crop_diagram_hint), style = MaterialTheme.typography.bodySmall)
        if(!crop.freehand) Text(stringResource(R.string.crop_freehand_hint), style = MaterialTheme.typography.bodySmall)
        if(crop.inputWidth > 0 && crop.inputHeight > 0) {
            CropAspectControls(selectedAspect, portrait, editable && draft != null,
                { selectAspect(it, portrait) }, { value ->
                    if(selectedAspect == "free") portrait = value else selectAspect(selectedAspect, value)
                })
            Text(stringResource(R.string.crop_aspect_dimensions, crop.inputWidth, crop.inputHeight),
                style = MaterialTheme.typography.bodySmall)
            Text(stringResource(R.string.crop_aspect_hint), style = MaterialTheme.typography.bodySmall)
            if(ratioError) Text(stringResource(R.string.crop_aspect_minimum), color = MaterialTheme.colorScheme.error,
                style = MaterialTheme.typography.bodySmall)
        }
        Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            CropCoordinate(left, { manualEdit { left = it } }, stringResource(R.string.crop_left), editable, draft != null, Modifier.weight(1f))
            CropCoordinate(top, { manualEdit { top = it } }, stringResource(R.string.crop_top), editable, draft != null, Modifier.weight(1f))
        }
        Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            CropCoordinate(right, { manualEdit { right = it } }, stringResource(R.string.crop_right), editable, draft != null, Modifier.weight(1f))
            CropCoordinate(bottom, { manualEdit { bottom = it } }, stringResource(R.string.crop_bottom), editable, draft != null, Modifier.weight(1f))
        }
        // Keep both edges coherent in the same native Float representation. A
        // slider only edits the draft; the complete rectangle is applied below.
        val bounds = draft ?: crop.bounds
        ParameterSlider(stringResource(R.string.crop_left), left.toEditorDoubleOrNull(), 0.0,
            maximumLeadingEdge(bounds.right, crop.minSize).toDouble(), editable && draft != null,
            onValue = { manualEdit { left = it.toFloat().toString() } })
        ParameterSlider(stringResource(R.string.crop_top), top.toEditorDoubleOrNull(), 0.0,
            maximumLeadingEdge(bounds.bottom, crop.minSize).toDouble(), editable && draft != null,
            onValue = { manualEdit { top = it.toFloat().toString() } })
        ParameterSlider(stringResource(R.string.crop_right), right.toEditorDoubleOrNull(),
            minimumTrailingEdge(bounds.left, crop.minSize).toDouble(), 1.0, editable && draft != null,
            onValue = { manualEdit { right = it.toFloat().toString() } })
        ParameterSlider(stringResource(R.string.crop_bottom), bottom.toEditorDoubleOrNull(),
            minimumTrailingEdge(bounds.top, crop.minSize).toDouble(), 1.0, editable && draft != null,
            onValue = { manualEdit { bottom = it.toFloat().toString() } })
        Text(stringResource(R.string.crop_coordinate_range, crop.minSize.toString()),
            style = MaterialTheme.typography.bodySmall)
        Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            TextButton(onClick = { manualEdit { left = "0.0"; top = "0.0"; right = "1.0"; bottom = "1.0" } }, enabled = editable) {
                Text(stringResource(R.string.crop_reset))
            }
            Button(shape = MaterialTheme.shapes.small,
                colors = ButtonDefaults.buttonColors(containerColor = MaterialTheme.colorScheme.primaryContainer,
                    contentColor = MaterialTheme.colorScheme.onPrimaryContainer), onClick = { draft?.let(onApply) }, enabled = editable && draft != null && changed) {
                Text(stringResource(R.string.crop_apply))
            }
        }
    }
}

@Composable
private fun CropAspectControls(selected: String, portrait: Boolean, editable: Boolean,
                               onPreset: (String) -> Unit, onPortrait: (Boolean) -> Unit) {
    var expanded by remember { mutableStateOf(false) }
    val free = stringResource(R.string.crop_aspect_free)
    val input = stringResource(R.string.crop_aspect_input)
    val title = stringResource(R.string.crop_aspect_title)
    Text(title, style = MaterialTheme.typography.labelLarge)
    Box {
        OutlinedButton(shape = MaterialTheme.shapes.small, onClick = { expanded = true }, enabled = editable,
            modifier = Modifier.semantics { contentDescription = title }) {
            Text(when(selected) { "free" -> free; "input" -> input; else -> selected })
        }
        DropdownMenu(expanded = expanded, onDismissRequest = { expanded = false }) {
            (listOf("free", "input") + cropAspectPresets.map { it.id }).forEach { preset ->
                DropdownMenuItem(text = { Text(when(preset) { "free" -> free; "input" -> input; else -> preset }) },
                    enabled = editable, onClick = { expanded = false; onPreset(preset) })
            }
        }
    }
    Row(verticalAlignment = androidx.compose.ui.Alignment.CenterVertically) {
        Text(stringResource(R.string.crop_aspect_orientation), Modifier.weight(1f))
        val description = stringResource(R.string.crop_aspect_orientation)
        Switch(checked = portrait, onCheckedChange = onPortrait, enabled = editable && selected != "1:1",
            modifier = Modifier.semantics { contentDescription = description })
    }
}

// Subtraction can round one ULP above the valid leading edge. Preserve the
// desktop's `trailing >= leading + minSize` test, including tiny minima.
private fun maximumLeadingEdge(trailing: Float, minSize: Float): Float {
    val candidate = (trailing - minSize).coerceAtMost(Math.nextDown(trailing)).coerceAtLeast(0f)
    return if(candidate + minSize > trailing) Math.nextDown(candidate).coerceAtLeast(0f) else candidate
}

private fun minimumTrailingEdge(leading: Float, minSize: Float): Float =
    (leading + minSize).coerceAtLeast(Math.nextUp(leading)).coerceAtMost(1f)

@Composable
private fun CropCoordinate(value: String, onValue: (String) -> Unit, label: String,
                           editable: Boolean, valid: Boolean, modifier: Modifier) {
    OutlinedTextField(value = value, onValueChange = onValue, label = { Text(label) }, modifier = modifier,
        enabled = editable, singleLine = true, isError = !valid,
        keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Ascii))
}

@Composable
private fun CropDiagram(applied: CropRect, draft: CropRect?, inputWidth: Int, inputHeight: Int) {
    val gridColor = MaterialTheme.colorScheme.outlineVariant
    val appliedColor = MaterialTheme.colorScheme.primary
    val draftColor = MaterialTheme.colorScheme.tertiary
    val description = stringResource(R.string.crop_diagram_description)
    Canvas(Modifier.fillMaxWidth().height(200.dp).background(MaterialTheme.colorScheme.surfaceVariant)
        .semantics { contentDescription = description }) {
        val padding = 12.dp.toPx()
        var width = (size.width - padding * 2).coerceAtLeast(1f)
        var height = (size.height - padding * 2).coerceAtLeast(1f)
        if(inputWidth > 0 && inputHeight > 0) {
            val aspect = inputWidth.toFloat() / inputHeight
            if(width / height > aspect) width = height * aspect else height = width / aspect
        }
        val x = (size.width - width) / 2f
        val y = (size.height - height) / 2f
        for(step in 0..4) {
            val fraction = step / 4f
            drawLine(gridColor, Offset(x + fraction * width, y), Offset(x + fraction * width, y + height))
            drawLine(gridColor, Offset(x, y + fraction * height), Offset(x + width, y + fraction * height))
        }
        fun origin(rect: CropRect) = Offset(x + rect.left * width, y + rect.top * height)
        fun extent(rect: CropRect) = Size((rect.right - rect.left) * width, (rect.bottom - rect.top) * height)
        // This is a normalized coordinate diagram. The already-cropped photo is
        // never drawn as though it were the uncropped crop-module input.
        drawRect(appliedColor, origin(applied), extent(applied), style = Stroke(2.dp.toPx()))
        if(draft != null && draft != applied) {
            drawRect(draftColor, origin(draft), extent(draft), style = Stroke(2.dp.toPx(),
                pathEffect = PathEffect.dashPathEffect(floatArrayOf(6.dp.toPx(), 4.dp.toPx()))))
        }
    }
}
