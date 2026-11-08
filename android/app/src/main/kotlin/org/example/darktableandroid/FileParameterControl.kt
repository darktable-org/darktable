/* SPDX-License-Identifier: GPL-3.0-or-later */
package org.example.darktableandroid

import android.net.Uri
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableLongStateOf
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.unit.dp
import org.example.darktableandroid.nativecore.ModuleTextParameter

@Composable
internal fun FileParameterControl(
    parameter: ModuleTextParameter, editable: Boolean, sessionId: Long,
    onSelected: (Uri, Long) -> Unit,
) {
    var pendingSessionId by rememberSaveable { mutableLongStateOf(-1L) }
    val picker = rememberLauncherForActivityResult(ActivityResultContracts.OpenDocument()) { uri ->
        val expectedSessionId = pendingSessionId
        pendingSessionId = -1L
        if(uri != null && expectedSessionId >= 0L) onSelected(uri, expectedSessionId)
    }
    Column(verticalArrangement = Arrangement.spacedBy(4.dp)) {
        Text(parameter.label, style = MaterialTheme.typography.labelLarge)
        val basename = parameter.value.substringAfterLast('/').substringAfterLast('\\')
        Text(basename.ifEmpty { stringResource(R.string.tool_file_empty) }, style = MaterialTheme.typography.bodySmall)
        Text(stringResource(R.string.tool_file_formats, parameter.extensions.joinToString(", ") { it.uppercase(java.util.Locale.ROOT) }),
            style = MaterialTheme.typography.bodySmall)
        OutlinedButton(shape = MaterialTheme.shapes.small, onClick = {
            pendingSessionId = sessionId
            // Android providers use inconsistent MIME types for LUT/PFM files;
            // the importer checks the advertised extensions and actual contents.
            picker.launch(arrayOf("*/*"))
        }, enabled = editable && sessionId >= 0L) { Text(stringResource(R.string.tool_file_choose)) }
    }
}
