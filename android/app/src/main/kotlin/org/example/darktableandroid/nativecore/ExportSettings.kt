/* SPDX-License-Identifier: GPL-3.0-or-later */
package org.example.darktableandroid.nativecore

import org.json.JSONArray
import org.json.JSONObject

/** Immutable values captured when the document picker is opened. */
data class ExportSettings(
    val maxWidth: Int = 0,
    val maxHeight: Int = 0,
    val quality: Int = 95,
    val pixelFormat: Int = 0,
    val pngCompression: Int = -1,
    val tiffCompression: Int = -1,
    val tiffCompressionLevel: Int = -1,
    val jpegSubsample: Int = -1,
    val dpi: Int = 0,
    val outputProfile: Int = 0,
    val renderingIntent: Int = 0,
    val metadataPolicy: Int = 0,
) {
    /** Order matches dt_mobile_export_settings after its native struct_size. */
    fun toNativeArray(): IntArray = intArrayOf(maxWidth, maxHeight, quality, pixelFormat,
        pngCompression, tiffCompression, tiffCompressionLevel, jpegSubsample, dpi,
        outputProfile, renderingIntent, metadataPolicy)

    companion object {
        fun fromNativeArray(values: IntArray): ExportSettings? {
            if(values.size != 12) return null
            return ExportSettings(values[0], values[1], values[2], values[3], values[4], values[5],
                values[6], values[7], values[8], values[9], values[10], values[11])
                .takeIf(::validExportSettings)
        }
    }
}

data class ExportOptionChoice(val value: Int, val label: String)
data class ExportNumberOption(val defaultValue: Int, val minimum: Int, val maximum: Int, val inherited: Int? = null) {
    fun accepts(value: Int): Boolean = value == defaultValue || value in minimum..maximum
    internal fun compatible(value: Int): Int = value.takeIf(::accepts) ?: defaultValue
}
data class ExportChoiceOption(val defaultValue: Int, val values: List<Int>, val inherited: Int? = null) {
    fun accepts(value: Int): Boolean = value in values
    internal fun compatible(value: Int): Int = value.takeIf(::accepts) ?: defaultValue
}

/** Capabilities are supplied by the installed encoder and profile registry. */
data class ExportOptions(
    val maxWidth: ExportNumberOption,
    val maxHeight: ExportNumberOption,
    val pixelFormats: List<ExportOptionChoice>,
    val pixelFormatDefault: Int,
    val profiles: List<ExportOptionChoice>,
    val profileDefault: Int,
    val intents: List<Int>,
    val intentDefault: Int,
    val metadataPolicies: List<ExportOptionChoice>,
    val metadataDefault: Int,
    val quality: ExportNumberOption? = null,
    val jpegSubsample: ExportChoiceOption? = null,
    val pngCompression: ExportNumberOption? = null,
    val tiffCompression: ExportChoiceOption? = null,
    val tiffCompressionLevel: ExportNumberOption? = null,
    val dpi: ExportNumberOption? = null,
) {
    fun defaultSettings(quality: Int? = null): ExportSettings = compatibleSettings(ExportSettings(
        maxWidth = maxWidth.defaultValue, maxHeight = maxHeight.defaultValue,
        quality = quality ?: this.quality?.defaultValue ?: 95,
        pixelFormat = pixelFormatDefault, pngCompression = pngCompression?.defaultValue ?: -1,
        tiffCompression = tiffCompression?.defaultValue ?: -1,
        tiffCompressionLevel = tiffCompressionLevel?.defaultValue ?: -1,
        jpegSubsample = jpegSubsample?.defaultValue ?: -1, dpi = dpi?.defaultValue ?: 0,
        outputProfile = profileDefault, renderingIntent = intentDefault, metadataPolicy = metadataDefault,
    ))

    /** Preserve supported preferences and clear encoder-specific values on a format change. */
    fun compatibleSettings(settings: ExportSettings): ExportSettings = settings.copy(
        maxWidth = maxWidth.compatible(settings.maxWidth),
        maxHeight = maxHeight.compatible(settings.maxHeight),
        quality = quality?.compatible(settings.quality) ?: settings.quality.takeIf { it in 5..100 } ?: 95,
        pixelFormat = settings.pixelFormat.takeIf { value -> pixelFormats.any { it.value == value } } ?: pixelFormatDefault,
        pngCompression = pngCompression?.compatible(settings.pngCompression) ?: -1,
        tiffCompression = tiffCompression?.compatible(settings.tiffCompression) ?: -1,
        tiffCompressionLevel = tiffCompressionLevel?.compatible(settings.tiffCompressionLevel) ?: -1,
        jpegSubsample = jpegSubsample?.compatible(settings.jpegSubsample) ?: -1,
        dpi = dpi?.compatible(settings.dpi) ?: 0,
        outputProfile = settings.outputProfile.takeIf { value -> profiles.any { it.value == value } } ?: profileDefault,
        renderingIntent = settings.renderingIntent.takeIf { it in intents } ?: intentDefault,
        metadataPolicy = settings.metadataPolicy.takeIf { value -> metadataPolicies.any { it.value == value } } ?: metadataDefault,
    )

    fun isValid(settings: ExportSettings): Boolean = validExportSettings(settings) &&
        maxWidth.accepts(settings.maxWidth) && maxHeight.accepts(settings.maxHeight) &&
        (quality?.accepts(settings.quality) != false) &&
        pixelFormats.any { it.value == settings.pixelFormat } &&
        (pngCompression?.accepts(settings.pngCompression) ?: (settings.pngCompression == -1)) &&
        (tiffCompression?.accepts(settings.tiffCompression) ?: (settings.tiffCompression == -1)) &&
        (tiffCompressionLevel?.accepts(settings.tiffCompressionLevel) ?: (settings.tiffCompressionLevel == -1)) &&
        (jpegSubsample?.accepts(settings.jpegSubsample) ?: (settings.jpegSubsample == -1)) &&
        (dpi?.accepts(settings.dpi) ?: (settings.dpi == 0)) &&
        profiles.any { it.value == settings.outputProfile } && settings.renderingIntent in intents &&
        metadataPolicies.any { it.value == settings.metadataPolicy }
}

private fun validExportSettings(settings: ExportSettings): Boolean =
    settings.maxWidth >= 0 && settings.maxHeight >= 0 && settings.quality in 5..100 &&
        settings.pixelFormat in 0..3 && settings.pngCompression in -1..9 && settings.tiffCompression in -1..2 &&
        (settings.tiffCompressionLevel == -1 || settings.tiffCompressionLevel in 1..9) &&
        settings.jpegSubsample in -1..4 && (settings.dpi == 0 || settings.dpi in 72..9600) &&
        settings.outputProfile in 0..7 && settings.renderingIntent in 0..4 && settings.metadataPolicy in 0..1

internal fun parseExportOptions(json: JSONObject): ExportOptions? = try {
    require(!json.getBoolean("upscale"))
    val pixelFormats = json.getJSONArray("pixelFormats").choices(0..3)
    val pixelDefault = json.integer("pixelFormatDefault").also { value -> require(pixelFormats.any { it.value == value }) }
    val profiles = json.getJSONArray("profiles").choices(0..7)
    val profileDefault = json.integer("profileDefault").also { value -> require(profiles.any { it.value == value }) }
    val intents = json.getJSONArray("intents").integers(0..4)
    val intentDefault = json.integer("intentDefault").also { require(it in intents) }
    val metadata = json.getJSONArray("metadataPolicies").choices(0..1)
    val metadataDefault = json.integer("metadataDefault").also { value -> require(metadata.any { it.value == value }) }
    ExportOptions(
        maxWidth = json.getJSONObject("maxWidth").numberOption(0..Int.MAX_VALUE),
        maxHeight = json.getJSONObject("maxHeight").numberOption(0..Int.MAX_VALUE),
        pixelFormats = pixelFormats, pixelFormatDefault = pixelDefault,
        profiles = profiles, profileDefault = profileDefault,
        intents = intents, intentDefault = intentDefault,
        metadataPolicies = metadata, metadataDefault = metadataDefault,
        quality = json.optionalObject("quality")?.numberOption(5..100),
        jpegSubsample = json.optionalObject("jpegSubsample")?.choiceOption(-1..4, -1),
        pngCompression = json.optionalObject("pngCompression")?.numberOption(0..9, -1),
        tiffCompression = json.optionalObject("tiffCompression")?.choiceOption(-1..2, -1),
        tiffCompressionLevel = json.optionalObject("tiffCompressionLevel")?.numberOption(1..9, -1),
        dpi = json.optionalObject("dpi")?.numberOption(72..9600, 0),
    ).also { require(it.isValid(it.defaultSettings())) }
} catch(_: org.json.JSONException) {
    null
} catch(_: IllegalArgumentException) {
    null
}

private fun JSONObject.optionalObject(name: String): JSONObject? = if(has(name)) getJSONObject(name) else null

private fun JSONObject.numberOption(domain: IntRange, inheritedDefault: Int? = null): ExportNumberOption {
    val minimum = integer("min")
    val maximum = integer("max")
    val default = integer("default")
    val inherited = if(has("inherited")) integer("inherited") else null
    require(minimum in domain && maximum in domain && minimum <= maximum)
    if(inheritedDefault == null) require(default in minimum..maximum && inherited == null)
    else require(default == inheritedDefault && inherited != null)
    // A malformed inherited config value should not hide the encoder's valid
    // explicit options; do not display or reuse it as an editable default.
    return ExportNumberOption(default, minimum, maximum, inherited?.takeIf { it in minimum..maximum })
}

private fun JSONObject.choiceOption(domain: IntRange, inheritedDefault: Int): ExportChoiceOption {
    val values = getJSONArray("values").integers(domain)
    val default = integer("default")
    val inherited = integer("inherited")
    require(default == inheritedDefault && default in values)
    return ExportChoiceOption(default, values, inherited.takeIf { it in values && it != inheritedDefault })
}

private fun JSONArray.choices(domain: IntRange): List<ExportOptionChoice> {
    require(length() in 1..64)
    return List(length()) { index ->
        val choice = getJSONObject(index)
        ExportOptionChoice(choice.integer("value").also { require(it in domain) }, choice.getString("label").also { require(it.isNotBlank()) })
    }.also { choices -> require(choices.map { it.value }.distinct().size == choices.size) }
}

private fun JSONArray.integers(domain: IntRange): List<Int> {
    require(length() in 1..64)
    return List(length()) { index -> get(index).exactInt().also { require(it in domain) } }
        .also { require(it.distinct().size == it.size) }
}

private fun JSONObject.integer(name: String): Int = get(name).exactInt()
private fun Any.exactInt(): Int {
    require(this is Number)
    val number = toDouble()
    require(number.isFinite() && number >= Int.MIN_VALUE.toDouble() && number <= Int.MAX_VALUE.toDouble())
    val result = number.toInt()
    require(number == result.toDouble())
    return result
}
