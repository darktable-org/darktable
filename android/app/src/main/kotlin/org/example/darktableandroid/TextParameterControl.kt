/* SPDX-License-Identifier: GPL-3.0-or-later */
package org.example.darktableandroid

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.unit.dp
import org.example.darktableandroid.nativecore.ModuleTextParameter
import org.example.darktableandroid.nativecore.accepts

@Composable
internal fun TextParameterControl(parameter: ModuleTextParameter, editable: Boolean, onValue: (String) -> Unit) {
    var draft by rememberSaveable(parameter.name, parameter.value) { mutableStateOf(parameter.value) }
    val valid = parameter.accepts(draft)
    val multiline = parameter.name == "text"
    Column(verticalArrangement = Arrangement.spacedBy(4.dp)) {
        OutlinedTextField(value = draft, onValueChange = { draft = it }, enabled = editable,
            label = { Text(parameter.label) }, modifier = Modifier.fillMaxWidth(),
            singleLine = !multiline, maxLines = if(multiline) 4 else 1, isError = !valid,
            supportingText = { if(!valid) Text(stringResource(R.string.parameter_text_invalid)) })
        Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            TextButton(onClick = { if(parameter.accepts(draft)) onValue(draft) },
                enabled = editable && valid && draft != parameter.value) {
                Text(stringResource(R.string.apply))
            }
            parameter.defaultValue?.takeIf(parameter::accepts)?.let { default ->
                val description = stringResource(R.string.parameter_reset_description, parameter.label)
                TextButton(onClick = {
                    draft = default
                    if(default != parameter.value) onValue(default)
                }, enabled = editable && (draft != default || parameter.value != default),
                    modifier = Modifier.semantics { contentDescription = description }) {
                    Text(stringResource(R.string.parameter_reset))
                }
            }
        }
    }
}
