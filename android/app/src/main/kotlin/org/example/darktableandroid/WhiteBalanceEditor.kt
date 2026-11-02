/* SPDX-License-Identifier: GPL-3.0-or-later */
package org.example.darktableandroid

import androidx.compose.foundation.layout.*
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.runtime.saveable.listSaver
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.ui.Modifier
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.semantics.stateDescription
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.unit.dp
import org.example.darktableandroid.nativecore.WhiteBalance
import org.example.darktableandroid.nativecore.WhiteBalanceCameraPresets
import org.example.darktableandroid.nativecore.WhiteBalanceTemperature
import org.example.darktableandroid.nativecore.whiteBalanceGains
import org.example.darktableandroid.nativecore.whiteBalanceTemperature
import kotlin.math.roundToInt

private val WhiteBalanceInputSaver = listSaver<List<String>, String>(save = { it }, restore = { it.toList() })

@Composable
internal fun WhiteBalanceEditor(balance: WhiteBalance, editable: Boolean,
                                onGains: (FloatArray) -> Unit, onSetting: (Int) -> Unit,
                                onTemperature: (Double, Double) -> Unit,
                                onPreset: (Int, Int) -> Unit = { _, _ -> }) {
    var input by rememberSaveable(balance.channels, balance.setting, stateSaver = WhiteBalanceInputSaver) {
        mutableStateOf(balance.channels.map { it.value.toString() })
    }
    var setting by rememberSaveable(balance.setting, balance.settings) {
        mutableIntStateOf(balance.settings.firstOrNull { it.id == balance.setting }?.id
            ?: balance.settings.firstOrNull()?.id ?: -1)
    }
    var expanded by remember { mutableStateOf(false) }
    val gains = whiteBalanceGains(balance, input)
    val changed = balance.setting != 2 || gains?.let { values ->
        balance.channels.any { values[it.index] != it.value }
    } == true
    val currentLabel = balance.cameraPresets?.let { catalog ->
        catalog.selectedTuning?.let { tuning ->
            catalog.presets.singleOrNull { it.id == catalog.selectedPreset && tuning in it.tuning }?.label
        }
    } ?: balance.settingLabel
    Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
        Text(stringResource(R.string.white_balance_current, currentLabel), style = MaterialTheme.typography.bodySmall)
        if(balance.settings.isNotEmpty()) {
            Box {
                OutlinedButton(onClick = { expanded = true }, enabled = editable) {
                    Text(balance.settings.firstOrNull { it.id == setting }?.label.orEmpty())
                }
                DropdownMenu(expanded = expanded, onDismissRequest = { expanded = false }) {
                    balance.settings.forEach { choice ->
                        DropdownMenuItem(text = { Text(choice.label) }, enabled = editable, onClick = {
                            setting = choice.id; expanded = false
                        })
                    }
                }
            }
            TextButton(onClick = { onSetting(setting) }, enabled = editable && balance.acceptsSetting(setting)
                && setting != balance.setting) { Text(stringResource(R.string.white_balance_apply_setting)) }
        }
        balance.cameraPresets?.let { catalog ->
            key(balance.channels, balance.setting, catalog) {
                WhiteBalanceCameraPresetEditor(catalog, editable, onPreset)
            }
        }
        balance.temperature?.let { temperature ->
            key(balance.channels, balance.setting) {
                WhiteBalanceTemperatureEditor(temperature, balance.setting, editable, onTemperature)
            }
        }
        Text(stringResource(R.string.white_balance_gains), style = MaterialTheme.typography.titleSmall)
        balance.channels.forEachIndexed { position, channel ->
            val value = input[position].toFloatOrNull()
            OutlinedTextField(value = input[position], onValueChange = { text ->
                input = input.mapIndexed { index, old -> if(index == position) text else old }
            }, label = { Text(channel.label) }, enabled = editable, singleLine = true,
                modifier = Modifier.fillMaxWidth(), isError = value == null || !value.isFinite() ||
                    value !in balance.manualMin..balance.manualMax,
                keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Ascii))
            ParameterSlider(channel.label, value?.toDouble(), balance.manualMin.toDouble(), balance.manualMax.toDouble(),
                editable, onValue = { updated ->
                    input = input.mapIndexed { index, old -> if(index == position) updated.toFloat().toString() else old }
                })
        }
        Text(stringResource(R.string.white_balance_gain_range, balance.manualMin.toString(), balance.manualMax.toString()),
            style = MaterialTheme.typography.bodySmall)
        TextButton(onClick = { gains?.let(onGains) }, enabled = editable && gains != null && changed) {
            Text(stringResource(R.string.white_balance_apply_gains))
        }
    }
}

@Composable
private fun WhiteBalanceCameraPresetEditor(catalog: WhiteBalanceCameraPresets, editable: Boolean,
                                          onPreset: (Int, Int) -> Unit) {
    var presetId by rememberSaveable(catalog) {
        mutableIntStateOf(catalog.selectedPreset ?: catalog.presets.first().id)
    }
    var tuning by rememberSaveable(catalog) {
        mutableIntStateOf(catalog.selectedTuning ?: catalog.presets.first().defaultTuning)
    }
    var expanded by remember { mutableStateOf(false) }
    val preset = catalog.presets.single { it.id == presetId }
    val presetDescription = stringResource(R.string.white_balance_camera_preset)
    val tuningDescription = stringResource(R.string.white_balance_fine_tune)
    Text(stringResource(R.string.white_balance_camera_presets), style = MaterialTheme.typography.titleSmall)
    Text(catalog.camera, style = MaterialTheme.typography.bodySmall)
    catalog.presets.singleOrNull { it.id == catalog.selectedPreset }?.let { current ->
        Text(stringResource(R.string.white_balance_current_preset, current.label, requireNotNull(catalog.selectedTuning)),
            style = MaterialTheme.typography.bodySmall)
    }
    Box {
        OutlinedButton(onClick = { expanded = true }, enabled = editable,
            modifier = Modifier.semantics { contentDescription = presetDescription }) {
            Text(preset.label)
        }
        DropdownMenu(expanded = expanded, onDismissRequest = { expanded = false }) {
            catalog.presets.forEach { choice ->
                DropdownMenuItem(text = { Text(choice.label) }, enabled = editable, onClick = {
                    presetId = choice.id
                    // Returning to the applied preset restores its applied tuning.
                    tuning = if(choice.id == catalog.selectedPreset) requireNotNull(catalog.selectedTuning)
                        else choice.defaultTuning
                    expanded = false
                })
            }
        }
    }
    Text(stringResource(R.string.white_balance_tuning_value, tuning))
    if(preset.tuning.size > 1) {
        // Slider positions index the native choices, rather than inventing
        // unsupported integers between sparse four-channel calibration values.
        Slider(value = preset.tuning.indexOf(tuning).toFloat(), valueRange = 0f..preset.tuning.lastIndex.toFloat(),
            steps = (preset.tuning.size - 2).coerceAtLeast(0), enabled = editable,
            onValueChange = { position -> tuning = preset.tuning[position.roundToInt().coerceIn(preset.tuning.indices)] },
            modifier = Modifier.fillMaxWidth().semantics {
                contentDescription = tuningDescription
                stateDescription = tuning.toString()
            })
    }
    TextButton(onClick = { onPreset(presetId, tuning) }, enabled = editable && catalog.accepts(presetId, tuning) &&
        (presetId != catalog.selectedPreset || tuning != catalog.selectedTuning)) {
        Text(stringResource(R.string.white_balance_apply_preset))
    }
}

@Composable
private fun WhiteBalanceTemperatureEditor(temperature: WhiteBalanceTemperature, setting: Int, editable: Boolean,
                                         onTemperature: (Double, Double) -> Unit) {
    var kelvin by rememberSaveable(temperature, setting) { mutableStateOf(temperature.kelvin.toString()) }
    var tint by rememberSaveable(temperature, setting) { mutableStateOf(temperature.tint.toString()) }
    val pair = whiteBalanceTemperature(temperature, kelvin, tint)
    Text(stringResource(R.string.white_balance_temperature), style = MaterialTheme.typography.titleSmall)
    OutlinedTextField(value = kelvin, onValueChange = { kelvin = it },
        label = { Text(stringResource(R.string.white_balance_kelvin)) }, enabled = editable, singleLine = true,
        modifier = Modifier.fillMaxWidth(), isError = kelvin.toDoubleOrNull()?.let {
            !it.isFinite() || it !in temperature.minKelvin..temperature.maxKelvin
        } != false, keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Ascii))
    ParameterSlider(stringResource(R.string.white_balance_kelvin), kelvin.toDoubleOrNull(),
        temperature.minKelvin, temperature.maxKelvin, editable, onValue = { kelvin = it.toString() })
    OutlinedTextField(value = tint, onValueChange = { tint = it },
        label = { Text(stringResource(R.string.white_balance_tint)) }, enabled = editable, singleLine = true,
        modifier = Modifier.fillMaxWidth(), isError = tint.toDoubleOrNull()?.let {
            !it.isFinite() || it !in temperature.minTint..temperature.maxTint
        } != false, keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Ascii))
    ParameterSlider(stringResource(R.string.white_balance_tint), tint.toDoubleOrNull(),
        temperature.minTint, temperature.maxTint, editable, onValue = { tint = it.toString() })
    Text(stringResource(R.string.white_balance_temperature_range, temperature.minKelvin.toString(),
        temperature.maxKelvin.toString(), temperature.minTint.toString(), temperature.maxTint.toString()),
        style = MaterialTheme.typography.bodySmall)
    // Arbitrary manual coefficients can have the same derived temperature but
    // a different green scale. Applying the displayed pair still selects the
    // desktop temperature representation and its green normalization.
    TextButton(onClick = { pair?.let { onTemperature(it.first, it.second) } }, enabled = editable && pair != null) {
        Text(stringResource(R.string.white_balance_apply_temperature))
    }
}
