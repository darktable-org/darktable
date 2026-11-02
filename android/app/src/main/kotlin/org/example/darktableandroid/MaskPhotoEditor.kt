/* SPDX-License-Identifier: GPL-3.0-or-later */
package org.example.darktableandroid

import android.graphics.Bitmap
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.gestures.detectTapGestures
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.heightIn
import androidx.compose.material3.DropdownMenu
import androidx.compose.material3.DropdownMenuItem
import androidx.compose.material3.FilterChip
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberUpdatedState
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.layout.onSizeChanged
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.onClick
import androidx.compose.ui.semantics.role
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.unit.IntSize
import androidx.compose.ui.unit.dp
import org.example.darktableandroid.nativecore.MaskPhotoPoint
import org.example.darktableandroid.nativecore.MaskPhotoPoints

/** A native projection belongs to this exact image, module instance and frame. */
data class MaskPhotoSelection(
    val sessionId: Long,
    val module: String,
    val preview: Bitmap,
    val points: MaskPhotoPoints,
)

/** A tap moves one native form centre or clone source in a single history edit. */
data class MaskPhotoTarget(
    val sessionId: Long,
    val module: String,
    val preview: Bitmap,
    val formId: Int,
    val source: Boolean,
)

@Composable
internal fun MaskPhotoEditor(
    enabled: Boolean,
    imageMode: Boolean,
    points: MaskPhotoPoints?,
    formId: Int?,
    source: Boolean,
    onImageMode: (Boolean) -> Unit,
    onForm: (Int) -> Unit,
    onSource: (Boolean) -> Unit,
) {
    val form = points?.forms?.singleOrNull { it.id == formId }
    var expanded by remember(imageMode, points) { mutableStateOf(false) }
    Column(Modifier.fillMaxWidth().testTag("mask_photo_editor"), verticalArrangement = Arrangement.spacedBy(8.dp)) {
        FilterChip(selected = imageMode, onClick = { onImageMode(!imageMode) }, enabled = enabled,
            label = { Text(stringResource(R.string.mask_photo_mode)) },
            modifier = Modifier.fillMaxWidth().heightIn(min = 48.dp).testTag("mask_photo_mode"))
        if(imageMode) {
            Text(stringResource(R.string.mask_photo_hint), style = MaterialTheme.typography.bodySmall)
            if(points == null) Text(stringResource(R.string.mask_photo_loading), style = MaterialTheme.typography.bodySmall)
            else if(points.forms.isEmpty()) Text(stringResource(R.string.mask_photo_empty), style = MaterialTheme.typography.bodySmall)
            else {
                Box(Modifier.fillMaxWidth()) {
                    val description = stringResource(R.string.mask_photo_choose_form)
                    OutlinedButton(onClick = { expanded = true }, enabled = enabled,
                        modifier = Modifier.fillMaxWidth().heightIn(min = 48.dp).testTag("mask_photo_form_picker")
                            .semantics { contentDescription = description }) {
                        Text(form?.label ?: description)
                    }
                    DropdownMenu(expanded = expanded, onDismissRequest = { expanded = false }) {
                        points.forms.forEach { candidate ->
                            DropdownMenuItem(text = { Text(candidate.label) }, enabled = enabled,
                                onClick = { expanded = false; onForm(candidate.id) },
                                modifier = Modifier.testTag("mask_photo_form_${candidate.id}"))
                        }
                    }
                }
                if(form != null) Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    if(form.center != null) FilterChip(selected = !source, onClick = { onSource(false) }, enabled = enabled,
                        label = { Text(stringResource(R.string.mask_photo_center)) },
                        modifier = Modifier.heightIn(min = 48.dp).testTag("mask_photo_center"))
                    if(form.source != null) FilterChip(selected = source, onClick = { onSource(true) }, enabled = enabled,
                        label = { Text(stringResource(R.string.mask_photo_source)) },
                        modifier = Modifier.heightIn(min = 48.dp).testTag("mask_photo_source"))
                }
            }
        }
    }
}

/** Uses the same fitted bitmap bounds as tone selection. The native projection
 * supplies the marker; stored mask coordinates never enter this overlay. */
@Composable
internal fun MaskPhotoTargetOverlay(
    imageWidth: Int,
    imageHeight: Int,
    enabled: Boolean,
    target: MaskPhotoTarget?,
    point: MaskPhotoPoint?,
    onPlace: (MaskPhotoTarget, Float, Float) -> Unit,
) {
    var size by remember { mutableStateOf(IntSize.Zero) }
    val bounds = fittedPreviewBounds(size.width.toFloat(), size.height.toFloat(), imageWidth, imageHeight)
    val place by rememberUpdatedState(onPlace)
    val description = stringResource(R.string.mask_photo_target)
    val centerAction = stringResource(R.string.mask_photo_place_middle)
    val canPlace = enabled && target != null
    Box(Modifier.fillMaxSize().onSizeChanged { size = it }.testTag("mask_photo_target")
        .pointerInput(canPlace, target, bounds) {
            if(canPlace) detectTapGestures { position ->
                bounds?.pointAt(position.x, position.y)?.let { tap -> target?.let { place(it, tap.x, tap.y) } }
            }
        }.semantics {
            contentDescription = description
            role = Role.Button
            if(canPlace) onClick(label = centerAction) { target?.let { place(it, .5f, .5f) }; true }
        }) {
        val marker = point?.takeIf { it.visible }?.let {
            val x = it.x; val y = it.y
            if(x != null && y != null && x.isFinite() && y.isFinite() && x >= 0f && x < 1f && y >= 0f && y < 1f)
                ToneEqualizerPoint(x, y) else null
        }
        if(marker != null && bounds != null) Canvas(Modifier.fillMaxSize().testTag("mask_photo_marker")) {
            val position = bounds.position(marker)
            val radius = 12.dp.toPx()
            drawCircle(Color.Black.copy(alpha = .8f), radius + 1.dp.toPx(), position, style = Stroke(4.dp.toPx()))
            drawCircle(Color.White, radius, position, style = Stroke(2.dp.toPx()))
            val extent = Offset(radius / 2f, 0f)
            val vertical = Offset(0f, radius / 2f)
            drawLine(Color.Black, position - extent, position + extent, 4.dp.toPx())
            drawLine(Color.Black, position - vertical, position + vertical, 4.dp.toPx())
            drawLine(Color.White, position - extent, position + extent, 2.dp.toPx())
            drawLine(Color.White, position - vertical, position + vertical, 2.dp.toPx())
        }
    }
}
