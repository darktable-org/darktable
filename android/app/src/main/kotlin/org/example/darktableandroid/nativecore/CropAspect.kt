/* SPDX-License-Identifier: GPL-3.0-or-later */
package org.example.darktableandroid.nativecore

import kotlin.math.max
import kotlin.math.min

internal data class CropAspectPreset(val id: String, val numerator: Int, val denominator: Int)

/** Positive landscape ratios; the editor's orientation switch takes the reciprocal. */
internal val cropAspectPresets = listOf(
    CropAspectPreset("1:1", 1, 1), CropAspectPreset("5:4", 5, 4),
    CropAspectPreset("4:3", 4, 3), CropAspectPreset("7:5", 7, 5),
    CropAspectPreset("3:2", 3, 2), CropAspectPreset("16:9", 16, 9),
    CropAspectPreset("2:1", 2, 1),
)

internal fun cropPresetAspect(id: String, inputWidth: Int, inputHeight: Int, portrait: Boolean): Double? {
    if(inputWidth <= 0 || inputHeight <= 0) return null
    val landscape = if(id == "input") {
        max(inputWidth, inputHeight).toDouble() / min(inputWidth, inputHeight)
    } else {
        val preset = cropAspectPresets.singleOrNull { it.id == id } ?: return null
        preset.numerator.toDouble() / preset.denominator
    }
    return if(portrait) 1.0 / landscape else landscape
}

/**
 * Fit an output-pixel aspect inside the reference crop, retaining its center.
 * Native crop coordinates are normalized separately by the input width/height;
 * using the ratio directly on these coordinates would be wrong on non-square inputs.
 * Very small references expand only enough to satisfy the native minimum.
 */
internal fun fitCropAspect(reference: CropRect, inputWidth: Int, inputHeight: Int,
                           outputAspect: Double, minSize: Float): CropRect? {
    if(!validCropRect(reference, minSize) || inputWidth <= 0 || inputHeight <= 0 ||
        !outputAspect.isFinite() || outputAspect <= 0.0) return null
    val normalizedAspect = outputAspect * inputHeight.toDouble() / inputWidth
    if(!normalizedAspect.isFinite() || normalizedAspect <= 0.0) return null
    val minimum = minSize.toDouble()
    var height = min((reference.bottom - reference.top).toDouble(),
        (reference.right - reference.left).toDouble() / normalizedAspect)
    height = max(height, max(minimum, minimum / normalizedAspect))
    var width = height * normalizedAspect
    if(!width.isFinite() || !height.isFinite() || width > 1.0 + 1e-12 || height > 1.0 + 1e-12) return null
    // Division and multiplication of the original input ratio can straddle 1
    // by one Double ULP; it still denotes the complete native input rectangle.
    width = min(width, 1.0)
    height = min(height, 1.0)
    val horizontal = cropAspectSpan((reference.left.toDouble() + reference.right) / 2.0, width, minSize) ?: return null
    val vertical = cropAspectSpan((reference.top.toDouble() + reference.bottom) / 2.0, height, minSize) ?: return null
    return CropRect(horizontal.first, vertical.first, horizontal.second, vertical.second)
        .takeIf { validCropRect(it, minSize) }
}

private fun cropAspectSpan(center: Double, extent: Double, minimum: Float): Pair<Float, Float>? {
    val leading = (center - extent / 2.0).coerceIn(0.0, 1.0 - extent).toFloat()
    var trailing = (leading.toDouble() + extent).coerceAtMost(1.0).toFloat()
    // Match the public/native Float minimum-edge test after coordinate rounding.
    trailing = max(trailing, max(leading + minimum, Math.nextUp(leading)))
    if(trailing > 1f) return null
    return leading to trailing
}
