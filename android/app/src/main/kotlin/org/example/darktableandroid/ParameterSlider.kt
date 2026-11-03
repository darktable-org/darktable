/* SPDX-License-Identifier: GPL-3.0-or-later */
package org.example.darktableandroid

import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.material3.Slider
import androidx.compose.material3.SliderDefaults
import androidx.compose.material3.MaterialTheme
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.semantics.stateDescription

/** Range mapping is UI only; all image processing and validation remain native.
 * Normalizing in Double avoids Float range overflow for very large native ranges.
 * onValue updates a local draft; callers commit only onFinished or explicit Apply. */
@Composable
internal fun ParameterSlider(label: String, value: Double?, minimum: Double, maximum: Double,
                             editable: Boolean, onValue: (Double) -> Unit,
                             onFinished: () -> Unit = {}) {
    if(!minimum.isFinite() || !maximum.isFinite() || maximum <= minimum) return
    val finite = value?.takeIf { it.isFinite() }
    val current = (finite ?: minimum).coerceIn(minimum, maximum)
    val fraction = ((current - minimum) / (maximum - minimum)).toFloat().coerceIn(0f, 1f)
    val description = stringResource(R.string.parameter_slider_description, label)
    Slider(value = fraction, valueRange = 0f..1f,
        colors = SliderDefaults.colors(thumbColor = MaterialTheme.colorScheme.primary,
            activeTrackColor = MaterialTheme.colorScheme.primaryContainer),
        onValueChange = { position ->
            // Use the exact range endpoint rather than accumulating arithmetic error.
            onValue(when {
                position <= 0f -> minimum
                position >= 1f -> maximum
                else -> minimum + position.toDouble() * (maximum - minimum)
            })
        }, onValueChangeFinished = onFinished, enabled = editable,
        modifier = Modifier.fillMaxWidth().semantics {
            contentDescription = description
            stateDescription = finite?.toString().orEmpty()
        })
}
