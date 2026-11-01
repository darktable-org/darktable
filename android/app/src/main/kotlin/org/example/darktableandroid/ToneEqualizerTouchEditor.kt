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
import androidx.compose.material3.Button
import androidx.compose.material3.FilterChip
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableDoubleStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.Alignment
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
import kotlin.math.min
import kotlin.math.round

/** A sample belongs to the exact native preview and module instance that produced it. */
data class ToneEqualizerSample(
    val sessionId: Long,
    val module: String,
    val preview: Bitmap,
    val x: Float,
    val y: Float,
    val exposure: Double,
)

internal data class ToneEqualizerPoint(val x: Float, val y: Float)

/** The bitmap's ContentScale.Fit rectangle, excluding padding and action bars. */
internal data class PreviewImageBounds(val left: Float, val top: Float, val width: Float, val height: Float) {
    fun pointAt(x: Float, y: Float): ToneEqualizerPoint? =
        if(x.isFinite() && y.isFinite() && x >= left && x < left + width && y >= top && y < top + height)
            ToneEqualizerPoint((x - left) / width, (y - top) / height) else null

    fun position(point: ToneEqualizerPoint): Offset = Offset(left + point.x * width, top + point.y * height)
}

internal fun fittedPreviewBounds(width: Float, height: Float, imageWidth: Int, imageHeight: Int): PreviewImageBounds? {
    if(!width.isFinite() || !height.isFinite() || width <= 0f || height <= 0f || imageWidth <= 0 || imageHeight <= 0)
        return null
    val scale = min(width / imageWidth, height / imageHeight)
    val fittedWidth = imageWidth * scale
    val fittedHeight = imageHeight * scale
    return PreviewImageBounds((width - fittedWidth) / 2f, (height - fittedHeight) / 2f, fittedWidth, fittedHeight)
}

internal fun toneEqualizerDraftOffset(value: Double): Double? =
    value.takeIf { it.isFinite() }?.let { round(it.coerceIn(-2.0, 2.0) * 100.0) / 100.0 }

/** A tap selects tone, while adjustment stays below the photo and commits once. */
@Composable
internal fun ToneEqualizerTouchEditor(
    enabled: Boolean,
    imageMode: Boolean,
    sample: ToneEqualizerSample?,
    onImageMode: (Boolean) -> Unit,
    onSelectCentre: () -> Unit,
    onAdjust: (ToneEqualizerSample, Double) -> Unit,
) {
    var draft by remember(sample, imageMode) { mutableDoubleStateOf(0.0) }
    Column(Modifier.fillMaxWidth().testTag("tone_equalizer_touch_editor"), verticalArrangement = Arrangement.spacedBy(8.dp)) {
        Text(stringResource(R.string.tone_touch_title), style = MaterialTheme.typography.titleSmall)
        Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.spacedBy(8.dp),
            verticalAlignment = Alignment.CenterVertically) {
            FilterChip(selected = imageMode, onClick = { onImageMode(!imageMode) }, enabled = enabled,
                label = { Text(stringResource(R.string.tone_touch_mode)) },
                modifier = Modifier.weight(1f).heightIn(min = 48.dp).testTag("tone_equalizer_image_mode"))
            if(imageMode) TextButton(onClick = onSelectCentre, enabled = enabled,
                modifier = Modifier.weight(1f).heightIn(min = 48.dp).testTag("tone_equalizer_select_centre")) {
                Text(stringResource(R.string.tone_touch_centre))
            }
        }
        if(imageMode) {
            Text(stringResource(R.string.tone_touch_hint), style = MaterialTheme.typography.bodySmall)
            if(sample != null) {
                Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
                    Column(Modifier.weight(1f)) {
                        Text(stringResource(R.string.tone_touch_sample, sample.exposure),
                            modifier = Modifier.testTag("tone_equalizer_sample"), style = MaterialTheme.typography.labelLarge)
                        Text(stringResource(R.string.tone_touch_offset, draft), style = MaterialTheme.typography.labelLarge)
                    }
                    TextButton(onClick = { draft = 0.0 }, enabled = enabled && draft != 0.0,
                        modifier = Modifier.heightIn(min = 48.dp).testTag("tone_equalizer_cancel_draft")) {
                        Text(stringResource(R.string.cancel))
                    }
                }
                ParameterSlider(stringResource(R.string.tone_touch_adjustment), draft, -2.0, 2.0, enabled,
                    onValue = { toneEqualizerDraftOffset(it)?.let { value -> draft = value } })
                Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    OutlinedButton(onClick = { draft = toneEqualizerDraftOffset(draft - .1) ?: draft },
                        enabled = enabled && draft > -2.0,
                        modifier = Modifier.weight(1f).heightIn(min = 48.dp).testTag("tone_equalizer_darker")) {
                        Text(stringResource(R.string.tone_touch_darker))
                    }
                    Button(onClick = { if(enabled && draft != 0.0) onAdjust(sample, draft) },
                        enabled = enabled && draft != 0.0,
                        modifier = Modifier.weight(1f).heightIn(min = 48.dp).testTag("tone_equalizer_apply")) {
                        Text(stringResource(R.string.apply))
                    }
                    OutlinedButton(onClick = { draft = toneEqualizerDraftOffset(draft + .1) ?: draft },
                        enabled = enabled && draft < 2.0,
                        modifier = Modifier.weight(1f).heightIn(min = 48.dp).testTag("tone_equalizer_brighter")) {
                        Text(stringResource(R.string.tone_touch_brighter))
                    }
                }
            } else Text(stringResource(R.string.tone_touch_choose_point), style = MaterialTheme.typography.bodySmall)
        }
    }
}

/** Shares the Image's content box; padding and action bars are outside this target. */
@Composable
internal fun ToneEqualizerPhotoTarget(
    imageWidth: Int,
    imageHeight: Int,
    enabled: Boolean,
    point: ToneEqualizerPoint?,
    onPoint: (ToneEqualizerPoint) -> Unit,
) {
    var size by remember { mutableStateOf(IntSize.Zero) }
    val bounds = fittedPreviewBounds(size.width.toFloat(), size.height.toFloat(), imageWidth, imageHeight)
    val description = stringResource(R.string.tone_touch_target)
    val centre = stringResource(R.string.tone_touch_centre)
    Box(Modifier.fillMaxSize().onSizeChanged { size = it }.testTag("tone_equalizer_photo_target")
        .pointerInput(enabled, bounds) {
            if(enabled) detectTapGestures { position -> bounds?.pointAt(position.x, position.y)?.let(onPoint) }
        }.semantics {
            contentDescription = description
            role = Role.Button
            if(enabled) onClick(label = centre) { onPoint(ToneEqualizerPoint(.5f, .5f)); true }
        }) {
        if(point != null && bounds != null) Canvas(Modifier.fillMaxSize()) {
            val centrePosition = bounds.position(point)
            val radius = 16.dp.toPx()
            drawCircle(Color.Black.copy(alpha = .8f), radius + 1.dp.toPx(), centrePosition, style = Stroke(4.dp.toPx()))
            drawCircle(Color.White, radius, centrePosition, style = Stroke(2.dp.toPx()))
            val gap = 5.dp.toPx()
            val length = 10.dp.toPx()
            for(sign in listOf(-1, 1)) {
                drawLine(Color.Black, centrePosition + Offset(sign * gap, 0f), centrePosition + Offset(sign * length, 0f), 4.dp.toPx())
                drawLine(Color.Black, centrePosition + Offset(0f, sign * gap), centrePosition + Offset(0f, sign * length), 4.dp.toPx())
                drawLine(Color.White, centrePosition + Offset(sign * gap, 0f), centrePosition + Offset(sign * length, 0f), 2.dp.toPx())
                drawLine(Color.White, centrePosition + Offset(0f, sign * gap), centrePosition + Offset(0f, sign * length), 2.dp.toPx())
            }
        }
    }
}
