/* SPDX-License-Identifier: GPL-3.0-or-later */
package org.example.darktableandroid.nativecore

import org.json.JSONObject

/** Availability comes from the native history owner, including its session boundary. */
data class EditHistory(val canUndo: Boolean, val canRedo: Boolean, val position: Int, val length: Int)

internal fun checkedEditHistory(canUndo: Boolean, canRedo: Boolean, position: Int, length: Int): EditHistory? =
    EditHistory(canUndo, canRedo, position, length).takeIf {
        length >= 0 && position in 0..length && (!canUndo || position > 0) && (!canRedo || position < length)
    }

internal fun parseEditHistory(json: String): EditHistory? = try {
    JSONObject(json).let {
        checkedEditHistory(it.getBoolean("canUndo"), it.getBoolean("canRedo"),
            it.getInt("position"), it.getInt("length"))
    }
} catch(_: org.json.JSONException) {
    null
}
