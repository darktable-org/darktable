/* SPDX-License-Identifier: GPL-3.0-or-later */
package org.example.darktableandroid

import androidx.compose.foundation.layout.*
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.unit.dp
import org.example.darktableandroid.nativecore.*

@Composable
internal fun ExportDialog(
    formats: List<ExportFormat>,
    initialFormatId: String,
    initialSettings: ExportSettings,
    enabled: Boolean,
    onDismiss: () -> Unit,
    onChoose: (ExportFormat, ExportSettings) -> Unit,
) {
    if(formats.isEmpty()) return
    var selectedId by rememberSaveable { mutableStateOf(initialFormatId) }
    var storedSettings by rememberSaveable { mutableStateOf(initialSettings.toNativeArray()) }
    val format = formats.firstOrNull { it.id == selectedId } ?: formats.first()
    val options = format.options
    val stored = ExportSettings.fromNativeArray(storedSettings) ?: ExportSettings()
    val settings = options?.compatibleSettings(stored) ?: ExportSettings(quality = stored.quality)
    var invalidFields by remember(format.id) { mutableStateOf(emptySet<String>()) }
    fun update(value: ExportSettings) { storedSettings = value.toNativeArray() }
    fun validField(name: String, valid: Boolean) {
        invalidFields = if(valid) invalidFields - name else invalidFields + name
    }

    AlertDialog(
        onDismissRequest = onDismiss,
        title = { Text(stringResource(R.string.export)) },
        text = {
            Column(Modifier.fillMaxWidth().heightIn(max = 520.dp).verticalScroll(rememberScrollState()),
                verticalArrangement = Arrangement.spacedBy(12.dp)) {
                Text(stringResource(R.string.full_resolution_export))
                ExportChoices(stringResource(R.string.export_format), format.id,
                    formats.map { it.id to it.name }, enabled) { id ->
                    val next = formats.first { it.id == id }
                    storedSettings = (next.options?.compatibleSettings(settings)
                        ?: ExportSettings(quality = settings.quality)).toNativeArray()
                    selectedId = id
                }
                if(options == null) {
                    // Older metadata still supports the original quality-only path.
                    if(format.id == "jpeg") ExportQuality(settings.quality, ExportNumberOption(95, 5, 100), enabled) {
                        update(settings.copy(quality = it))
                    }
                } else key(format.id) {
                    Text(stringResource(R.string.export_size_title), style = MaterialTheme.typography.titleSmall)
                    Text(stringResource(R.string.export_size_hint), style = MaterialTheme.typography.bodySmall)
                    ExportInteger(stringResource(R.string.export_width), settings.maxWidth, options.maxWidth,
                        enabled, { validField("width", it) }) { update(settings.copy(maxWidth = it)) }
                    ExportInteger(stringResource(R.string.export_height), settings.maxHeight, options.maxHeight,
                        enabled, { validField("height", it) }) { update(settings.copy(maxHeight = it)) }

                    HorizontalDivider()
                    Text(stringResource(R.string.export_encoding_title), style = MaterialTheme.typography.titleSmall)
                    if(options.pixelFormats.size > 1) ExportChoices(stringResource(R.string.export_pixel_format),
                        settings.pixelFormat, options.pixelFormats.map { it.value to it.label }, enabled) {
                        update(settings.copy(pixelFormat = it))
                    }
                    options.quality?.let { quality ->
                        ExportQuality(settings.quality, quality, enabled) { update(settings.copy(quality = it)) }
                    }
                    options.jpegSubsample?.let { subsample ->
                        ExportChoices(stringResource(R.string.export_subsample), settings.jpegSubsample,
                            subsample.values.mapNotNull { value -> subsampleLabel(value)?.let { value to it } }, enabled) {
                            update(settings.copy(jpegSubsample = it))
                        }
                    }
                    options.pngCompression?.let { compression ->
                        ExportInheritedInteger(stringResource(R.string.export_compression), settings.pngCompression,
                            compression, enabled, { validField("pngCompression", it) }) {
                            update(settings.copy(pngCompression = it))
                        }
                    }
                    options.tiffCompression?.let { compression ->
                        ExportChoices(stringResource(R.string.export_compression), settings.tiffCompression,
                            compression.values.mapNotNull { value -> compressionLabel(value)?.let { value to it } }, enabled) {
                            update(settings.copy(tiffCompression = it))
                        }
                    }
                    options.tiffCompressionLevel?.let { compression ->
                        ExportInheritedInteger(stringResource(R.string.export_compression_level), settings.tiffCompressionLevel,
                            compression, enabled, { validField("tiffCompressionLevel", it) }) {
                            update(settings.copy(tiffCompressionLevel = it))
                        }
                    }
                    options.dpi?.let { dpi ->
                        ExportInheritedInteger(stringResource(R.string.export_dpi), settings.dpi, dpi,
                            enabled, { validField("dpi", it) }) { update(settings.copy(dpi = it)) }
                    }

                    HorizontalDivider()
                    Text(stringResource(R.string.export_color_title), style = MaterialTheme.typography.titleSmall)
                    ExportChoices(stringResource(R.string.export_profile), settings.outputProfile,
                        options.profiles.map { it.value to it.label }, enabled) {
                        update(settings.copy(outputProfile = it))
                    }
                    ExportChoices(stringResource(R.string.export_intent), settings.renderingIntent,
                        options.intents.mapNotNull { value -> intentLabel(value)?.let { value to it } }, enabled) {
                        update(settings.copy(renderingIntent = it))
                    }
                    HorizontalDivider()
                    ExportChoices(stringResource(R.string.export_metadata), settings.metadataPolicy,
                        options.metadataPolicies.map { it.value to it.label }, enabled) {
                        update(settings.copy(metadataPolicy = it))
                    }
                    if(settings.metadataPolicy == 1) Text(stringResource(R.string.export_metadata_hint),
                        style = MaterialTheme.typography.bodySmall)
                }
            }
        },
        confirmButton = {
            TextButton(onClick = { onChoose(format, settings) }, enabled = enabled && invalidFields.isEmpty()
                && (options?.isValid(settings) ?: true)) { Text(stringResource(R.string.choose_destination)) }
        },
        dismissButton = { TextButton(onClick = onDismiss) { Text(stringResource(R.string.cancel)) } },
    )
}

@Composable
private fun ExportQuality(value: Int, option: ExportNumberOption, enabled: Boolean, onValue: (Int) -> Unit) {
    Text(stringResource(R.string.export_quality, value))
    Slider(value = value.toFloat(), onValueChange = { onValue(it.toInt()) }, enabled = enabled,
        valueRange = option.minimum.toFloat()..option.maximum.toFloat(),
        steps = (option.maximum - option.minimum - 1).coerceIn(0, 1000))
}

@Composable
private fun ExportInheritedInteger(label: String, value: Int, option: ExportNumberOption, enabled: Boolean,
                                   onValid: (Boolean) -> Unit, onValue: (Int) -> Unit) {
    Column(verticalArrangement = Arrangement.spacedBy(4.dp)) {
        Text(label, style = MaterialTheme.typography.labelLarge)
        Row(verticalAlignment = Alignment.CenterVertically) {
            Checkbox(checked = value == option.defaultValue, enabled = enabled, onCheckedChange = { inherit ->
                onValid(true)
                onValue(if(inherit) option.defaultValue else
                    (option.inherited ?: option.minimum).coerceIn(option.minimum, option.maximum))
            })
            Text(option.inherited?.let { stringResource(R.string.export_inherit_value, it) }
                ?: stringResource(R.string.export_inherit_setting))
        }
        if(value != option.defaultValue) ExportInteger(label, value, option, enabled, onValid, onValue)
    }
}

@Composable
private fun ExportInteger(label: String, value: Int, option: ExportNumberOption, enabled: Boolean,
                          onValid: (Boolean) -> Unit, onValue: (Int) -> Unit) {
    var text by remember(value, option) { mutableStateOf(value.toString()) }
    val parsed = text.toIntOrNull()?.takeIf { it in option.minimum..option.maximum }
    OutlinedTextField(value = text, onValueChange = {
        text = it
        val changed = it.toIntOrNull()?.takeIf { candidate -> candidate in option.minimum..option.maximum }
        onValid(changed != null)
        changed?.let(onValue)
    }, label = { Text(label) }, modifier = Modifier.fillMaxWidth(), enabled = enabled,
        singleLine = true, isError = parsed == null, keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Number),
        supportingText = { if(parsed == null) Text(stringResource(R.string.export_integer_range, option.minimum, option.maximum)) })
}

@Composable
private fun <T> ExportChoices(label: String, value: T, choices: List<Pair<T, String>>, enabled: Boolean,
                              onValue: (T) -> Unit) {
    if(choices.isEmpty()) return
    var expanded by remember { mutableStateOf(false) }
    Column(verticalArrangement = Arrangement.spacedBy(4.dp)) {
        Text(label, style = MaterialTheme.typography.labelLarge)
        Box {
            OutlinedButton(shape = MaterialTheme.shapes.small, onClick = { expanded = true }, enabled = enabled) {
                Text(choices.firstOrNull { it.first == value }?.second.orEmpty())
            }
            DropdownMenu(expanded = expanded, onDismissRequest = { expanded = false }) {
                choices.forEach { choice ->
                    DropdownMenuItem(text = { Text(choice.second) }, enabled = enabled,
                        onClick = { expanded = false; onValue(choice.first) })
                }
            }
        }
    }
}

@Composable
private fun subsampleLabel(value: Int): String? = when(value) {
    -1 -> stringResource(R.string.export_inherit_setting)
    0 -> stringResource(R.string.export_automatic)
    1 -> "4:4:4"
    2 -> "4:4:0"
    3 -> "4:2:2"
    4 -> "4:2:0"
    else -> null
}

@Composable
private fun compressionLabel(value: Int): String? = when(value) {
    -1 -> stringResource(R.string.export_inherit_setting)
    0 -> stringResource(R.string.export_uncompressed)
    1 -> stringResource(R.string.export_deflate)
    2 -> stringResource(R.string.export_deflate_predictor)
    else -> null
}

@Composable
private fun intentLabel(value: Int): String? = when(value) {
    0 -> stringResource(R.string.export_image_intent)
    1 -> stringResource(R.string.export_perceptual)
    2 -> stringResource(R.string.export_relative)
    3 -> stringResource(R.string.export_saturation)
    4 -> stringResource(R.string.export_absolute)
    else -> null
}
