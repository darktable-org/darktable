/* SPDX-License-Identifier: GPL-3.0-or-later */
package org.example.darktableandroid

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Box
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.unit.dp
import org.example.darktableandroid.nativecore.CurveOptionChoice
import org.example.darktableandroid.nativecore.RgbCurveOptions

@Composable
internal fun RgbCurveOptionsEditor(options: RgbCurveOptions, editable: Boolean, onApply: (Int, Int) -> Unit) {
    var mode by rememberSaveable(options.mode, options.preserveColors) { mutableIntStateOf(options.mode) }
    var preserve by rememberSaveable(options.mode, options.preserveColors) { mutableIntStateOf(options.preserveColors) }
    Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
        CurveOptionPicker(stringResource(R.string.curve_mode), mode, options.modes, editable) { mode = it }
        if(mode == 0) CurveOptionPicker(stringResource(R.string.curve_preserve_colors), preserve,
            options.preserveColorChoices, editable) { preserve = it }
        TextButton(onClick = { onApply(mode, preserve) }, enabled = editable && options.accepts(mode, preserve) &&
            (mode != options.mode || preserve != options.preserveColors)) {
            Text(stringResource(R.string.curve_apply_options))
        }
    }
}

@Composable
private fun CurveOptionPicker(label: String, value: Int, choices: List<CurveOptionChoice>,
                              editable: Boolean, onValue: (Int) -> Unit) {
    var expanded by remember { mutableStateOf(false) }
    Column {
        Text(label, style = MaterialTheme.typography.labelLarge)
        Box {
            OutlinedButton(shape = MaterialTheme.shapes.small, onClick = { expanded = true }, enabled = editable) {
                Text(choices.firstOrNull { it.value == value }?.label.orEmpty())
            }
            DropdownMenu(expanded = expanded, onDismissRequest = { expanded = false }) {
                choices.forEach { choice ->
                    DropdownMenuItem(text = { Text(choice.label) }, onClick = { onValue(choice.value); expanded = false },
                        enabled = editable)
                }
            }
        }
    }
}
