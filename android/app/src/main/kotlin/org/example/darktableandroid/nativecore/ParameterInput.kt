/* SPDX-License-Identifier: GPL-3.0-or-later */
package org.example.darktableandroid.nativecore

/** Match the native storage precision before checking bounds or the applied value. */
internal fun ModuleParameter.parseNumericInput(text: String): Double? {
    val parsed = text.toEditorDoubleOrNull() ?: return null
    val value = if(type == "float") parsed.toFloat().toDouble() else parsed
    if(!value.isFinite() || (type == "int" && value % 1.0 != 0.0)) return null
    return value.takeIf {
        (minimum == null || it >= minimum) && (maximum == null || it <= maximum)
    }
}
