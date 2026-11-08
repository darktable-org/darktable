/* SPDX-License-Identifier: GPL-3.0-or-later */
package org.example.darktableandroid.nativecore

data class CropRect(val left: Float, val top: Float, val right: Float, val bottom: Float)

internal fun validCropRect(rect: CropRect, minSize: Float): Boolean =
    minSize.isFinite() && minSize > 0f && minSize <= 1f &&
        rect.left.isFinite() && rect.top.isFinite() && rect.right.isFinite() && rect.bottom.isFinite() &&
        rect.left in 0f..1f && rect.top in 0f..1f && rect.right in 0f..1f && rect.bottom in 0f..1f &&
        rect.right > rect.left && rect.bottom > rect.top &&
        // The desktop widget and native adapter calculate minimum edges in Float.
        rect.right >= rect.left + minSize && rect.bottom >= rect.top + minSize

/** Normalize numeric input to the same float coordinates sent to the native editor. */
internal fun parseCropInput(left: String, top: String, right: String, bottom: String, minSize: Float): CropRect? {
    val rect = CropRect(left.toEditorFloatOrNull() ?: return null, top.toEditorFloatOrNull() ?: return null,
        right.toEditorFloatOrNull() ?: return null, bottom.toEditorFloatOrNull() ?: return null)
    return rect.takeIf { validCropRect(it, minSize) }
}
