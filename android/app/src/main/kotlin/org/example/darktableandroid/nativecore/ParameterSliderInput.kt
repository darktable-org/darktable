/* SPDX-License-Identifier: GPL-3.0-or-later */
package org.example.darktableandroid.nativecore

import kotlin.math.round

/** Desktop soft bounds only affect dragging; precise input keeps the native hard bounds. */
internal fun ModuleParameter.sliderBounds(): ClosedFloatingPointRange<Double>? {
    val low = minimum?.takeIf { it.isFinite() } ?: return null
    val high = maximum?.takeIf { it.isFinite() && it > low } ?: return null
    val softLow = sliderMinimum
    val softHigh = sliderMaximum
    val softValid = softLow != null && softHigh != null && softLow.isFinite() && softHigh.isFinite() &&
        softLow >= low && softHigh <= high && softLow < softHigh
    val start = if(softValid) checkNotNull(softLow) else low
    val end = if(softValid) checkNotNull(softHigh) else high
    // An imported or precisely entered value outside the usual desktop range
    // remains visible and can be dragged back without clamping the actual edit.
    val applied = value.takeIf { it.isFinite() && it in low..high }
    return minOf(start, applied ?: start)..maxOf(end, applied ?: end)
}

internal fun ModuleParameter.sliderInput(value: Double): String = when(type) {
    "float" -> value.toFloat().toString()
    "int" -> round(value).toLong().toString()
    else -> value.toString()
}

/** Never invent defaults: this value comes from the original module's default_params. */
internal fun ModuleParameter.resetValue(): Double? {
    val candidate = defaultValue ?: return null
    if(type == "bool" || type == "boolean") return candidate.takeIf { it == 0.0 || it == 1.0 }
    if(choices.isNotEmpty()) return candidate.takeIf { value -> choices.any { it.value == value } }
    return parseNumericInput(candidate.toString())
}
