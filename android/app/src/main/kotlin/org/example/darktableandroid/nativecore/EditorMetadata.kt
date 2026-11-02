/* SPDX-License-Identifier: GPL-3.0-or-later */
package org.example.darktableandroid.nativecore

import org.json.JSONArray
import org.json.JSONObject

data class ParameterChoice(val value: Double, val label: String)
data class ModuleParameter(
    val name: String,
    val label: String,
    val type: String,
    val value: Double,
    val minimum: Double?,
    val maximum: Double?,
    val choices: List<ParameterChoice>,
    val sliderMinimum: Double? = null,
    val sliderMaximum: Double? = null,
    val defaultValue: Double? = null,
)
data class ModuleTextParameter(
    val name: String,
    val label: String,
    val value: String,
    /** Maximum UTF-8 payload bytes, excluding the native terminating NUL. */
    val maxBytes: Int,
    val defaultValue: String? = null,
    val isFile: Boolean = false,
    val extensions: List<String> = emptyList(),
)
data class EditorModule(
    val operation: String,
    val name: String,
    val enabled: Boolean,
    val canDisable: Boolean,
    val parameters: List<ModuleParameter>,
    val curves: List<ModuleCurve> = emptyList(),
    val crop: ModuleCrop? = null,
    val rgbCurveOptions: RgbCurveOptions? = null,
    val whiteBalance: WhiteBalance? = null,
    val canCreateInstance: Boolean = false,
    val canReset: Boolean = false,
    val textParameters: List<ModuleTextParameter> = emptyList(),
    val lensCorrection: LensCorrection? = null,
)
data class ModuleCrop(
    val bounds: CropRect, val minSize: Float, val freehand: Boolean,
    /** Full crop-module input ROI. Zero means the runtime did not provide it. */
    val inputWidth: Int = 0, val inputHeight: Int = 0,
)
data class ModuleCurve(
    val channel: Int,
    val label: String,
    val editable: Boolean,
    val interpolation: Int,
    val maxNodes: Int,
    val minSpacing: Float,
    val nodes: List<CurvePoint>,
    val samples: List<CurvePoint>,
    val resetNodes: List<CurvePoint>? = null,
)
data class ExportFormat(
    val id: String,
    val name: String,
    val extension: String,
    val mimeType: String,
    val bitDepth: Int?,
    val options: ExportOptions? = null,
)

internal val EditorModule.hasEditableControls: Boolean
    get() = parameters.isNotEmpty() || textParameters.isNotEmpty() || curves.any { it.editable } || crop != null ||
        rgbCurveOptions != null || whiteBalance != null

/** Presentation order only; native pixelpipe order remains unchanged. */
internal fun List<EditorModule>.controlsFirst(): List<EditorModule> = sortedBy { !it.hasEditableControls }

internal fun List<EditorModule>.initialControlModule(): String? =
    firstOrNull { it.operation == "exposure" && it.hasEditableControls }?.operation
        ?: firstOrNull { it.hasEditableControls }?.operation

internal fun parseModules(json: String): List<EditorModule> = JSONArray(json).let { modules ->
    List(modules.length()) { index ->
        val module = modules.getJSONObject(index)
        val parameters = module.optJSONArray("parameters") ?: JSONArray()
        val curves = module.optJSONArray("curves") ?: JSONArray()
        val textParameters = module.optJSONArray("textParameters") ?: JSONArray()
        EditorModule(module.getString("operation"), module.getString("name"), module.getBoolean("enabled"), module.optBoolean("canDisable", true),
            List(parameters.length()) { parameterIndex ->
                val parameter = parameters.getJSONObject(parameterIndex)
                val choices = parameter.optJSONArray("choices") ?: JSONArray()
                ModuleParameter(
                    parameter.getString("name"), parameter.optString("label", parameter.getString("name")),
                    parameter.getString("type"), parameter.getDouble("value"),
                    parameter.finiteNumber("min"), parameter.finiteNumber("max"),
                    List(choices.length()) { choiceIndex ->
                        val choice = choices.getJSONObject(choiceIndex)
                        ParameterChoice(choice.getDouble("value"), choice.getString("label"))
                    },
                    parameter.finiteNumber("sliderMin"), parameter.finiteNumber("sliderMax"),
                    parameter.finiteNumber("default"),
                )
            },
            // An unavailable or older curve schema must not hide the image's
            // remaining modules and supported controls.
            (0 until curves.length()).mapNotNull { curveIndex ->
                curves.optJSONObject(curveIndex)?.readCurve()
            }.distinctBy { it.channel },
            module.optJSONObject("crop")?.readCrop(),
            module.optJSONObject("rgbCurveOptions")?.readRgbCurveOptions(),
            module.optJSONObject("whiteBalance")?.readWhiteBalance(),
            // Older runtimes and malformed optional capabilities stay unavailable.
            module.opt("canCreateInstance") == true,
            module.opt("canReset") == true,
            (0 until textParameters.length()).mapNotNull { textIndex ->
                textParameters.optJSONObject(textIndex)?.readTextParameter()
            }.distinctBy { it.name },
            module.optJSONObject("lensCorrection")?.readLensCorrection(),
        )
    }
}

/** Match native instance IDs exactly; a retired instance must not target its base module. */
internal fun List<EditorModule>.instanceSource(identifier: String): EditorModule? =
    singleOrNull { it.operation == identifier }?.takeIf { it.canCreateInstance }

internal fun retainModuleSelection(modules: List<EditorModule>, identifier: String?): String? =
    identifier?.takeIf { id -> modules.count { it.operation == id } == 1 }

internal fun parseExportFormats(json: String): List<ExportFormat> = JSONArray(json).let { formats ->
    List(formats.length()) { index ->
        val format = formats.getJSONObject(index)
        ExportFormat(
            format.optString("id", format.getString("name")),
            format.optString("label", format.getString("name")),
            format.getString("extension"),
            format.optString("mimeType", "application/octet-stream").takeIf { '/' in it } ?: "application/octet-stream",
            format.optInt("bitDepth", 0).takeIf { it > 0 },
            format.optJSONObject("options")?.let(::parseExportOptions),
        )
    }
}

private fun JSONObject.finiteNumber(name: String): Double? =
    if(has(name) && !isNull(name)) optDouble(name).takeIf { it.isFinite() } else null

private fun JSONObject.readTextParameter(): ModuleTextParameter? = try {
    val name = getString("name")
    val limit = curveOptionInt("maxBytes")
    require(name.isNotEmpty() && limit in 1..65535)
    val value = get("value") as? String ?: throw IllegalArgumentException("Expected text")
    val file = opt("file")
    require(file == null || file == JSONObject.NULL || file is Boolean)
    val isFile = file == true
    val allowedExtensions = optJSONArray("extensions")?.let { items ->
        (0 until items.length()).mapNotNull { index ->
            (items.opt(index) as? String)?.lowercase(java.util.Locale.ROOT)?.removePrefix(".")
                ?.takeIf { it.length in 1..16 && it.all { char -> char in 'a'..'z' || char in '0'..'9' } }
        }.distinct()
    } ?: emptyList()
    require((name != "file.import" || isFile) && (!isFile || allowedExtensions.isNotEmpty()))
    ModuleTextParameter(name, optString("label", name), value, limit, opt("default") as? String,
        isFile, allowedExtensions).let {
        if(it.defaultValue != null && !it.accepts(it.defaultValue)) it.copy(defaultValue = null) else it
    }
} catch(_: org.json.JSONException) {
    null
} catch(_: IllegalArgumentException) {
    null
}

private fun JSONObject.readWhiteBalance(): WhiteBalance? = try {
    val channels = getJSONArray("channels")
    val settings = getJSONArray("settings")
    require(channels.length() in 3..4 && settings.length() in 0..3)
    checkedWhiteBalance(List(channels.length()) { index ->
        val channel = channels.getJSONObject(index)
        WhiteBalanceChannel(channel.curveOptionInt("index"), channel.getString("label"), channel.getDouble("value").toFloat())
    }, getDouble("manualMin").toFloat(), getDouble("manualMax").toFloat(), curveOptionInt("setting"),
        getString("settingLabel"), List(settings.length()) { index ->
            val choice = settings.getJSONObject(index)
            WhiteBalanceSetting(choice.curveOptionInt("id"), choice.getString("label"))
        }, optJSONObject("temperature")?.readWhiteBalanceTemperature(),
        optJSONObject("cameraPresets")?.readWhiteBalanceCameraPresets())
} catch(_: org.json.JSONException) {
    null
} catch(_: IllegalArgumentException) {
    null
}

private fun JSONObject.readWhiteBalanceTemperature(): WhiteBalanceTemperature? = try {
    checkedWhiteBalanceTemperature(getDouble("kelvin"), getDouble("tint"), getDouble("minKelvin"),
        getDouble("maxKelvin"), getDouble("minTint"), getDouble("maxTint"))
} catch(_: org.json.JSONException) {
    null
}

private fun JSONObject.readWhiteBalanceCameraPresets(): WhiteBalanceCameraPresets? = try {
    val presets = getJSONArray("presets")
    require(presets.length() in 1..50)
    fun selected(name: String): Int? = if(isNull(name)) null else curveOptionInt(name)
    checkedWhiteBalanceCameraPresets(getString("camera"), List(presets.length()) { index ->
        val preset = presets.getJSONObject(index)
        val tuning = preset.getJSONArray("tuning")
        require(tuning.length() in 1..4096)
        WhiteBalancePreset(preset.curveOptionInt("id"), preset.getString("label"),
            List(tuning.length()) { tuningIndex ->
                val value = (tuning.get(tuningIndex) as? Number)?.toDouble()
                    ?: throw IllegalArgumentException("Expected a tuning integer")
                require(value.isFinite() && value == value.toInt().toDouble())
                value.toInt()
            }, preset.curveOptionInt("defaultTuning"))
    }, selected("selectedPreset"), selected("selectedTuning"))
} catch(_: org.json.JSONException) {
    null
} catch(_: IllegalArgumentException) {
    null
}

private fun JSONObject.readRgbCurveOptions(): RgbCurveOptions? = try {
    checkedRgbCurveOptions(curveOptionInt("mode"), curveOptionInt("preserveColors"),
        getJSONArray("modes").curveOptionChoices(), getJSONArray("preserveColorChoices").curveOptionChoices())
} catch(_: org.json.JSONException) {
    null
} catch(_: IllegalArgumentException) {
    null
}

private fun JSONObject.curveOptionInt(name: String): Int {
    val value = (get(name) as? Number)?.toDouble() ?: throw IllegalArgumentException("Expected an integer")
    require(value.isFinite() && value == value.toInt().toDouble())
    return value.toInt()
}

private fun JSONArray.curveOptionChoices(): List<CurveOptionChoice> {
    require(length() in 1..64)
    return List(length()) { index ->
        val choice = getJSONObject(index)
        CurveOptionChoice(choice.curveOptionInt("value"), choice.getString("label"))
    }
}

private fun JSONObject.readCrop(): ModuleCrop? = try {
    val bounds = CropRect(getDouble("left").toFloat(), getDouble("top").toFloat(),
        getDouble("right").toFloat(), getDouble("bottom").toFloat())
    val minSize = getDouble("minSize").toFloat()
    require(validCropRect(bounds, minSize))
    val inputWidth = cropInputDimension("inputWidth")
    val inputHeight = cropInputDimension("inputHeight")
    val dimensionsAvailable = inputWidth > 0 && inputHeight > 0
    ModuleCrop(bounds, minSize, getBoolean("freehand"),
        if(dimensionsAvailable) inputWidth else 0, if(dimensionsAvailable) inputHeight else 0)
} catch(_: org.json.JSONException) {
    null
} catch(_: IllegalArgumentException) {
    null
}

/** Optional capabilities never replace valid legacy bounds with guessed dimensions. */
private fun JSONObject.cropInputDimension(name: String): Int {
    val number = (opt(name) as? Number)?.toDouble() ?: return 0
    return if(number.isFinite() && number >= 1.0 && number <= Int.MAX_VALUE.toDouble() && number % 1.0 == 0.0)
        number.toInt() else 0
}

private fun JSONObject.readCurve(): ModuleCurve? = try {
    val nodes = getJSONArray("nodes").curvePoints()
    val maxNodes = getInt("maxNodes")
    val minSpacing = optDouble("minSpacing", 0.000001).toFloat()
    require(validCurveNodes(nodes, maxNodes, minSpacing))
    val interpolation = getInt("interpolation")
    val channel = getInt("channel")
    require(interpolation in 0..2 && channel in 0..2)
    val samples = getJSONArray("samples").curvePoints()
    require(samples.size in 2..4096 && samples.all { it.x in 0f..1f } &&
        samples.zipWithNext().all { (left, right) -> right.x > left.x })
    val resetNodes = optJSONArray("resetNodes")?.let { reset ->
        try { reset.curvePoints().takeIf { validCurveNodes(it, maxNodes, minSpacing) } }
        catch(_: org.json.JSONException) { null }
        catch(_: IllegalArgumentException) { null }
    }
    ModuleCurve(channel, getString("label"), getBoolean("editable"), interpolation,
        maxNodes, minSpacing, nodes, samples, resetNodes)
} catch(_: org.json.JSONException) {
    null
} catch(_: IllegalArgumentException) {
    null
}

private fun JSONArray.curvePoints(): List<CurvePoint> = List(length()) { index ->
    val point = getJSONObject(index)
    val x = point.getDouble("x").toFloat()
    val y = point.getDouble("y").toFloat()
    require(x.isFinite() && y.isFinite()) { "Invalid native curve coordinates" }
    CurvePoint(x, y)
}
