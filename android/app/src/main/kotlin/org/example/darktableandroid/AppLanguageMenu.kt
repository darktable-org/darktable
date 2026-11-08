/* SPDX-License-Identifier: GPL-3.0-or-later */
package org.example.darktableandroid

import android.app.Activity
import android.content.Context
import android.content.ContextWrapper
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.material3.DropdownMenu
import androidx.compose.material3.DropdownMenuItem
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalConfiguration
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics

@Composable
internal fun AppLanguageMenu(
    selected: AppLanguage? = null,
    onSelect: ((AppLanguage) -> Unit)? = null,
) {
    val context = LocalContext.current
    val configuration = LocalConfiguration.current
    val language = selected ?: remember(configuration) { AppLanguages.selected(context) }
    var expanded by remember { mutableStateOf(false) }
    Column {
        Text(stringResource(R.string.app_language), style = MaterialTheme.typography.titleSmall)
        Text(stringResource(R.string.app_language_description), style = MaterialTheme.typography.bodySmall)
        Box {
            val description = stringResource(R.string.app_language_choose)
            OutlinedButton(onClick = { expanded = true }, shape = MaterialTheme.shapes.small,
                modifier = Modifier.fillMaxWidth().testTag("app_language_picker")
                    .semantics { contentDescription = description }) {
                Text(stringResource(language.label))
            }
            DropdownMenu(expanded = expanded, onDismissRequest = { expanded = false }) {
                AppLanguage.entries.forEach { choice ->
                    DropdownMenuItem(text = { Text(stringResource(choice.label)) },
                        modifier = Modifier.testTag("app_language_${choice.name}"), onClick = {
                            expanded = false
                            if(onSelect != null) onSelect(choice)
                            else context.activity()?.let { AppLanguages.select(it, choice) }
                        })
                }
            }
        }
    }
}

private tailrec fun Context.activity(): Activity? = when(this) {
    is Activity -> this
    is ContextWrapper -> baseContext.activity()
    else -> null
}
