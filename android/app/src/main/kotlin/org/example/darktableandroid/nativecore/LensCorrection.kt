/* SPDX-License-Identifier: GPL-3.0-or-later */
package org.example.darktableandroid.nativecore

import org.json.JSONArray
import org.json.JSONObject

data class LensProfileChoice(val value: String, val label: String)

/** A description of the original correction processor, never an inferred calibration. */
data class LensCorrection(
    val status: String,
    val message: String?,
    val method: Int,
    val embeddedAvailable: Boolean,
    val databaseAvailable: Boolean,
    val cameraMatched: Boolean,
    val lensMatched: Boolean,
    /** Original correction bits: TCA=1, vignetting=2, distortion=4. */
    val supportedFlags: Int,
    val camera: String,
    val lens: String,
    val cameras: List<LensProfileChoice>,
    val lenses: List<LensProfileChoice>,
    val imageCamera: String = "",
    val imageLens: String = "",
)

internal fun JSONObject.readLensCorrection(): LensCorrection? = try {
    fun text(name: String, maximum: Int): String {
        val value = get(name) as? String ?: throw IllegalArgumentException("Expected text")
        require(value.length <= maximum && '\u0000' !in value)
        return value
    }
    fun flag(name: String): Boolean = get(name) as? Boolean ?: throw IllegalArgumentException("Expected boolean")
    fun optionalText(name: String): String = if(!has(name) || isNull(name)) "" else text(name, 512)
    fun integer(name: String, domain: IntRange): Int {
        val value = (get(name) as? Number)?.toDouble() ?: throw IllegalArgumentException("Expected integer")
        require(value.isFinite() && value == value.toInt().toDouble() && value.toInt() in domain)
        return value.toInt()
    }
    val status = text("status", 128).also { require(it.isNotBlank()) }
    val message = opt("message").takeIf { it != null && it != JSONObject.NULL }?.let {
        require(it is String && it.length <= 4096 && '\u0000' !in it)
        (it as String).takeIf(String::isNotBlank)
    }
    LensCorrection(status, message, integer("method", 0..2), flag("embeddedAvailable"),
        flag("databaseAvailable"), flag("cameraMatched"), flag("lensMatched"), integer("supportedFlags", 0..7),
        text("camera", 512), text("lens", 512), optJSONArray("cameras").readLensChoices(),
        optJSONArray("lenses").readLensChoices(), optionalText("imageCamera"), optionalText("imageLens"))
} catch(_: org.json.JSONException) {
    null
} catch(_: IllegalArgumentException) {
    null
}

private fun JSONArray?.readLensChoices(): List<LensProfileChoice> {
    if(this == null) return emptyList()
    require(length() <= 20000)
    return (0 until length()).map { index ->
        val item = getJSONObject(index)
        val value = item.get("value") as? String ?: throw IllegalArgumentException("Expected profile value")
        val label = item.get("label") as? String ?: throw IllegalArgumentException("Expected profile label")
        require(value.isNotBlank() && value.length <= 512 && '\u0000' !in value)
        require(label.isNotBlank() && label.length <= 1024 && '\u0000' !in label)
        LensProfileChoice(value, label)
    }.distinctBy { it.value }
}
