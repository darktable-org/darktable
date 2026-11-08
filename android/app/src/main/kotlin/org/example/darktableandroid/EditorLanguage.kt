/* SPDX-License-Identifier: GPL-3.0-or-later */
package org.example.darktableandroid

import androidx.compose.runtime.Composable
import androidx.compose.runtime.State
import androidx.compose.runtime.produceState
import androidx.compose.ui.platform.LocalConfiguration
import androidx.compose.ui.platform.LocalContext
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import org.example.darktableandroid.nativecore.GettextCatalog
import org.example.darktableandroid.nativecore.catalogLocaleTag
import org.example.darktableandroid.nativecore.localized
import org.example.darktableandroid.nativecore.withMobileOverrides

/** Read the desktop's existing translation assets without changing native process locale. */
@Composable
internal fun rememberEditorCatalog(): State<GettextCatalog> {
    val context = LocalContext.current
    val configuration = LocalConfiguration.current
    val locale = if(configuration.locales.isEmpty) "en" else configuration.locales[0].toLanguageTag()
    val catalogLocale = catalogLocaleTag(locale)
    return produceState(GettextCatalog.empty(), context, catalogLocale) {
        value = withContext(Dispatchers.IO) {
            val catalog = catalogLocale?.let { language ->
                runCatching {
                    context.assets.open("darktable-runtime/locale/$language/LC_MESSAGES/darktable.mo").use {
                        GettextCatalog.parse(it.readBytes())
                    }
                }.getOrNull()
            } ?: GettextCatalog.empty()
            catalog.withMobileOverrides(context)
        }
    }
}

/** Display copies retain the exact native session, bitmap, IDs, values and history. */
internal fun EditorState.forDisplay(catalog: GettextCatalog): EditorState = when(this) {
    is EditorState.Ready -> localizedReady(catalog)
    is EditorState.Working -> copy(previous = previous?.localizedReady(catalog))
    is EditorState.Failed -> copy(message = catalog.translate(message))
    EditorState.Empty -> this
}

private fun EditorState.Ready.localizedReady(catalog: GettextCatalog): EditorState.Ready = copy(
    modules = modules.map { it.localized(catalog) },
    exportFormats = exportFormats.map { it.localized(catalog) },
    notice = notice?.let(catalog::translate),
)
