/* SPDX-License-Identifier: GPL-3.0-or-later */
package org.example.darktableandroid.nativecore

import org.json.JSONException
import org.json.JSONObject

/** Coordinates projected by the native pixelpipe into one exact preview. */
data class MaskPhotoPoint(val x: Float?, val y: Float?, val visible: Boolean)
data class MaskPhotoForm(
    val id: Int,
    val label: String,
    val type: String,
    val center: MaskPhotoPoint?,
    val source: MaskPhotoPoint?,
)
data class MaskPhotoPoints(val forms: List<MaskPhotoForm>)

/** Invalid optional forms never become editable targets. A point can remain
 * placeable when its current projection is outside the preview or unavailable. */
fun parseMaskPhotoPoints(json: String): MaskPhotoPoints? {
    return try {
        val forms = JSONObject(json).optJSONArray("forms") ?: return null
        val parsed = (0 until forms.length()).mapNotNull { index ->
            forms.optJSONObject(index)?.readMaskPhotoForm()
        }
        val counts = parsed.groupingBy { it.id }.eachCount()
        MaskPhotoPoints(parsed.filter { counts[it.id] == 1 })
    } catch(_: JSONException) {
        null
    } catch(_: IllegalArgumentException) {
        null
    }
}

private fun JSONObject.readMaskPhotoForm(): MaskPhotoForm? {
    val rawId = opt("id") as? Number ?: return null
    val id = rawId.toDouble().takeIf { it.isFinite() && it > 0 && it <= Int.MAX_VALUE && it % 1.0 == 0.0 }
        ?.toInt() ?: return null
    val label = (opt("label") as? String)?.takeIf { it.isNotBlank() } ?: return null
    val type = (opt("type") as? String)?.takeIf { it in setOf("circle", "ellipse", "gradient", "path", "brush") }
        ?: return null
    val center = optJSONObject("center")?.readMaskPhotoPoint()
    val source = optJSONObject("source")?.readMaskPhotoPoint()
    if(center == null && source == null) return null
    return MaskPhotoForm(id, label, type, center, source)
}

private fun JSONObject.readMaskPhotoPoint(): MaskPhotoPoint? {
    val visible = opt("visible") as? Boolean ?: return null
    fun coordinate(name: String): Float? = (opt(name) as? Number)?.toDouble()
        ?.takeIf { it.isFinite() }?.toFloat()?.takeIf { it.isFinite() }
    val x = coordinate("x")
    val y = coordinate("y")
    return MaskPhotoPoint(x, y, visible && x != null && y != null && x >= 0f && x < 1f && y >= 0f && y < 1f)
}
