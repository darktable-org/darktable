/* SPDX-License-Identifier: GPL-3.0-or-later */
package org.example.darktableandroid

import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.text.KeyboardActions
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.material3.FilterChip
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.platform.LocalSoftwareKeyboardController
import androidx.compose.ui.text.input.ImeAction
import androidx.compose.ui.unit.dp
import org.example.darktableandroid.nativecore.EditorModule

/** Search covers native parameter labels as well as tool names, including instances. */
internal fun filterEditingTools(modules: List<EditorModule>, query: String, enabledOnly: Boolean): List<EditorModule> {
    val words = query.trim().split(Regex("\\s+")).filter { it.isNotEmpty() }
    return modules.filter { module ->
        (!enabledOnly || module.enabled) && (words.isEmpty() || run {
            val labels = buildString {
                append(module.name).append(' ').append(module.operation)
                module.parameters.forEach { append(' ').append(it.label).append(' ').append(it.name) }
                module.textParameters.forEach { append(' ').append(it.label).append(' ').append(it.name) }
                module.curves.forEach { append(' ').append(it.label) }
            }
            words.all { labels.contains(it, ignoreCase = true) }
        })
    }
}

/** A tool-name term narrows tools; remaining terms narrow the controls inside the card. */
internal fun controlSearchWords(module: EditorModule, query: String): List<String> {
    val identity = "${module.name} ${module.operation}"
    return query.trim().split(Regex("\\s+")).filter { it.isNotEmpty() && !identity.contains(it, ignoreCase = true) }
}

internal fun matchesControlSearch(words: List<String>, name: String, label: String): Boolean =
    words.all { "$name $label".contains(it, ignoreCase = true) }

@Composable
internal fun EditingToolSearch(
    query: String,
    onQuery: (String) -> Unit,
    enabledOnly: Boolean,
    onEnabledOnly: (Boolean) -> Unit,
    visibleCount: Int,
    totalCount: Int,
) {
    val keyboard = LocalSoftwareKeyboardController.current
    Column(verticalArrangement = Arrangement.spacedBy(4.dp)) {
        OutlinedTextField(value = query, onValueChange = onQuery,
            modifier = Modifier.fillMaxWidth(), singleLine = true,
            keyboardOptions = KeyboardOptions(imeAction = ImeAction.Search),
            keyboardActions = KeyboardActions(onSearch = { keyboard?.hide() }),
            label = { Text(stringResource(R.string.tools_search)) },
            trailingIcon = {
                if(query.isNotEmpty()) TextButton(onClick = { onQuery("") }) { Text(stringResource(R.string.tools_clear_search)) }
            })
        Row(Modifier.horizontalScroll(rememberScrollState()), horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            FilterChip(selected = !enabledOnly, onClick = { onEnabledOnly(false) }, label = { Text(stringResource(R.string.tools_all)) })
            FilterChip(selected = enabledOnly, onClick = { onEnabledOnly(true) }, label = { Text(stringResource(R.string.tools_enabled)) })
        }
        Text(stringResource(R.string.tools_visible_count, visibleCount, totalCount), style = MaterialTheme.typography.labelSmall)
    }
}
