/* SPDX-License-Identifier: GPL-3.0-or-later */
package org.example.darktableandroid.nativecore

import java.util.IllegalFormatException
import java.util.Locale

private val nativeLabelSeparators = listOf(" · ", " / ")
private val numberedNativeLabel = Regex("^(.+?) ([0-9]+)$")

/** Fixed native array labels combine translated text with unchanged component indexes. */
internal fun GettextCatalog.translateLabel(message: String, context: String? = null): String {
    val exact = translate(message, context)
    if(exact != message) return exact
    for(separator in nativeLabelSeparators) {
        if(separator in message) return message.split(separator).joinToString(separator) { translateLabel(it, context) }
    }
    val numbered = numberedNativeLabel.matchEntire(message)
    if(numbered != null) {
        val prefix = numbered.groupValues[1]
        val translated = translate(prefix, context)
        if(translated != prefix) return "$translated ${numbered.groupValues[2]}"
    }
    if(message.endsWith(" output")) {
        val component = message.removeSuffix(" output")
        val translated = translate(component, context)
        if(translated != component) return try {
            String.format(Locale.ROOT, translate("%s output"), translated)
        } catch(_: IllegalFormatException) {
            message
        }
    }
    return message
}

/** Display copies only: all native addresses, numeric data and user content remain exact. */
internal fun EditorModule.localized(catalog: GettextCatalog): EditorModule {
    val separator = name.indexOf(" · ")
    val displayName = if(separator < 0) catalog.translate(name)
        else catalog.translate(name.substring(0, separator)) + name.substring(separator)
    val context = when(operation.substringBefore('@')) {
        "temperature" -> "white balance"
        else -> null
    }
    return copy(
        name = displayName,
        parameters = parameters.map { parameter -> parameter.copy(
            label = catalog.translateLabel(parameter.label, context),
            choices = parameter.choices.map { it.copy(label = catalog.translate(it.label, context)) },
        ) },
        textParameters = textParameters.map { it.copy(label = catalog.translateLabel(it.label, context)) },
        curves = curves.map { it.copy(label = catalog.translateLabel(it.label, context)) },
        whiteBalance = whiteBalance?.let { balance -> balance.copy(
            channels = balance.channels.map { it.copy(label = catalog.translate(it.label)) },
            settingLabel = catalog.translate(balance.settingLabel, "white balance"),
            settings = balance.settings.map { it.copy(label = catalog.translate(it.label, "white balance")) },
            cameraPresets = balance.cameraPresets?.let { presets -> presets.copy(
                presets = presets.presets.map { it.copy(label = catalog.translate(it.label, "white balance")) },
            ) },
        ) },
    )
}

internal fun ExportFormat.localized(catalog: GettextCatalog): ExportFormat = copy(
    name = catalog.translate(name),
    options = options?.let { original -> original.copy(
        pixelFormats = original.pixelFormats.map { it.copy(label = catalog.translate(it.label)) },
        metadataPolicies = original.metadataPolicies.map { it.copy(label = catalog.translate(it.label)) },
        // ICC/profile names are native identities or user-provided names, not UI text.
    ) },
)
