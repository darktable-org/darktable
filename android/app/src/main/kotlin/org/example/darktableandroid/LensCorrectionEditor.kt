/* SPDX-License-Identifier: GPL-3.0-or-later */
package org.example.darktableandroid

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.key
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.unit.dp
import org.example.darktableandroid.nativecore.LensCorrection
import org.example.darktableandroid.nativecore.LensProfileChoice
import org.example.darktableandroid.nativecore.ModuleParameter
import org.example.darktableandroid.nativecore.ModuleTextParameter
import org.example.darktableandroid.nativecore.accepts

/** Uses only profiles and capabilities supplied by the original Lensfun processor. */
@Composable
internal fun LensCorrectionEditor(
    info: LensCorrection,
    controls: LensCorrectionControls,
    editable: Boolean,
    onParameter: (String, Double) -> Unit,
    onTextParameter: (String, String) -> Unit,
) {
    Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
        controls.method?.let { parameter ->
            ParameterControl(parameter, editable) { onParameter(parameter.name, it) }
        }
        val unmatchedCamera = info.method == 1 && info.databaseAvailable && !info.cameraMatched
        Text(when {
            unmatchedCamera && info.lensMatched -> lensStatusMessage("unverified_camera")
            unmatchedCamera -> stringResource(R.string.lens_status_missing_camera)
            else -> info.message ?: lensStatusMessage(info.status)
        },
            style = MaterialTheme.typography.bodySmall,
            color = if((info.method == 1 && (!info.cameraMatched || !info.lensMatched)) || (info.method == 0 && !info.embeddedAvailable))
                MaterialTheme.colorScheme.error else MaterialTheme.colorScheme.onSurfaceVariant)
        controls.actions.filter { it.name == "lens.autodetect" }.forEach { parameter ->
            ParameterControl(parameter, editable) { onParameter(parameter.name, it) }
        }
        if(info.method == 1) {
            if(info.imageCamera.isNotBlank())
                Text(stringResource(R.string.lens_image_camera, info.imageCamera), style = MaterialTheme.typography.bodySmall)
            if(info.imageLens.isNotBlank())
                Text(stringResource(R.string.lens_image_lens, info.imageLens), style = MaterialTheme.typography.bodySmall)
            LensProfilePicker(stringResource(R.string.lens_camera_profile), info.camera, info.cameras,
                controls.cameraProfile, editable, info.cameraMatched,
                stringResource(R.string.lens_camera_profile_unmatched)) { name, value -> onTextParameter(name, value) }
            LensProfilePicker(stringResource(R.string.lens_profile), info.lens,
                if(info.cameraMatched) info.lenses else emptyList(),
                controls.lensProfile, editable && info.cameraMatched, info.cameraMatched && info.lensMatched,
                stringResource(if(info.cameraMatched) R.string.lens_profile_unselected else R.string.lens_profile_requires_camera)
            ) { name, value -> onTextParameter(name, value) }
            Text(stringResource(R.string.lens_profile_names_hint), style = MaterialTheme.typography.bodySmall)
            if(unmatchedCamera && info.lens.isNotBlank())
                Text(stringResource(R.string.lens_unverified_stored_lens, info.lens),
                    style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.error)
            if(info.databaseAvailable && (info.cameras.isEmpty() || info.lenses.isEmpty()) && editable)
                Text(stringResource(R.string.lens_profile_none), style = MaterialTheme.typography.bodySmall)
        }
        if(info.method != 2 && (info.method != 1 || info.cameraMatched)) {
            val capabilities = listOf(4 to R.string.lens_distortion, 1 to R.string.lens_tca, 2 to R.string.lens_vignetting)
                .filter { info.supportedFlags and it.first != 0 }.map { stringResource(it.second) }
            if(capabilities.isNotEmpty()) Text(stringResource(R.string.lens_available_corrections,
                capabilities.joinToString(", ")), style = MaterialTheme.typography.bodySmall)
            if(info.method == 0 && info.supportedFlags and 1 != 0 && info.supportedFlags and 4 == 0)
                Text(stringResource(R.string.lens_ca_warp_hint), style = MaterialTheme.typography.bodySmall)
        }
        if(controls.shooting.isNotEmpty()) {
            Text(stringResource(R.string.lens_shooting_settings), style = MaterialTheme.typography.titleSmall)
            Text(stringResource(R.string.lens_shooting_hint), style = MaterialTheme.typography.bodySmall)
            LensParameters(controls.shooting, editable, onParameter)
        }
        if(controls.correction.isNotEmpty()) {
            HorizontalDivider()
            Text(stringResource(R.string.lens_correction_settings), style = MaterialTheme.typography.titleSmall)
            LensParameters(controls.correction, editable, onParameter)
        }
        controls.actions.filter { it.name != "lens.autodetect" }.forEach { parameter ->
            ParameterControl(parameter, editable) { onParameter(parameter.name, it) }
        }
        if(controls.manualVignette.isNotEmpty()) {
            HorizontalDivider()
            Text(stringResource(R.string.lens_manual_vignette), style = MaterialTheme.typography.titleSmall)
            Text(stringResource(R.string.lens_manual_vignette_hint), style = MaterialTheme.typography.bodySmall)
            LensParameters(controls.manualVignette, editable, onParameter)
        }
    }
}

@Composable
private fun lensStatusMessage(status: String): String = when(status) {
    "manual" -> stringResource(R.string.lens_status_manual)
    "ready" -> stringResource(R.string.lens_status_ready)
    "missing_database" -> stringResource(R.string.lens_status_missing_database)
    "missing_lens" -> stringResource(R.string.lens_status_missing_lens)
    "unverified_camera" -> stringResource(R.string.lens_status_unverified_camera)
    "missing_settings" -> stringResource(R.string.lens_status_missing_settings)
    "no_calibration" -> stringResource(R.string.lens_status_no_calibration)
    else -> status.replace('_', ' ').replaceFirstChar { it.uppercase() }
}

@Composable
private fun LensParameters(parameters: List<ModuleParameter>, editable: Boolean,
                           onParameter: (String, Double) -> Unit) {
    parameters.forEach { parameter ->
        key(parameter.name) { ParameterControl(parameter, editable) { onParameter(parameter.name, it) } }
    }
}

@Composable
private fun LensProfilePicker(
    title: String,
    current: String,
    choices: List<LensProfileChoice>,
    control: ModuleTextParameter?,
    editable: Boolean,
    matched: Boolean,
    placeholder: String,
    onSelect: (String, String) -> Unit,
) {
    var open by remember { mutableStateOf(false) }
    var query by remember { mutableStateOf("") }
    val allowed = remember(choices, control) { choices.filter { control?.accepts(it.value) == true } }
    Column(verticalArrangement = Arrangement.spacedBy(4.dp)) {
        Text(title, style = MaterialTheme.typography.labelLarge)
        OutlinedButton(shape = MaterialTheme.shapes.small, onClick = { query = ""; open = true }, enabled = editable && allowed.isNotEmpty(),
            modifier = Modifier.fillMaxWidth().semantics { contentDescription = title }) {
            Text(if(matched) choices.firstOrNull { it.value == current }?.label ?: current.ifBlank { placeholder }
                else placeholder)
        }
    }
    if(open) AlertDialog(onDismissRequest = { open = false },
        title = { Text(title) },
        confirmButton = { TextButton(onClick = { open = false }) { Text(stringResource(R.string.done)) } },
        text = {
            Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
                OutlinedTextField(value = query, onValueChange = { query = it }, singleLine = true,
                    label = { Text(stringResource(R.string.lens_profile_search)) }, modifier = Modifier.fillMaxWidth())
                val filtered = remember(allowed, query) {
                    val words = query.trim().split(Regex("\\s+")).filter(String::isNotEmpty)
                    allowed.filter { choice -> words.all { choice.label.contains(it, ignoreCase = true) ||
                        choice.value.contains(it, ignoreCase = true) } }
                }
                if(filtered.isEmpty()) Text(stringResource(R.string.lens_profile_no_matches))
                LazyColumn(Modifier.fillMaxWidth().heightIn(max = 260.dp)) {
                    items(filtered, key = { it.value }) { choice ->
                        TextButton(onClick = {
                            if(editable && control != null && control.accepts(choice.value)) {
                                open = false
                                onSelect(control.name, choice.value)
                            }
                        }, enabled = editable && choice.value != current, modifier = Modifier.fillMaxWidth()) {
                            Row(Modifier.fillMaxWidth()) { Text(choice.label) }
                        }
                    }
                }
            }
        })
}
