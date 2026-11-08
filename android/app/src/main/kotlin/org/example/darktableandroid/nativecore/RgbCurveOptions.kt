/* SPDX-License-Identifier: GPL-3.0-or-later */
package org.example.darktableandroid.nativecore

data class CurveOptionChoice(val value: Int, val label: String)
data class RgbCurveOptions(
    val mode: Int,
    val preserveColors: Int,
    val modes: List<CurveOptionChoice>,
    val preserveColorChoices: List<CurveOptionChoice>,
) {
    fun accepts(mode: Int, preserveColors: Int): Boolean =
        mode in 0..1 && preserveColors in 0..6 &&
            modes.any { it.value == mode } && preserveColorChoices.any { it.value == preserveColors }
}

internal fun checkedRgbCurveOptions(mode: Int, preserveColors: Int,
    modes: List<CurveOptionChoice>, preserveColorChoices: List<CurveOptionChoice>): RgbCurveOptions? {
    fun valid(choices: List<CurveOptionChoice>, domain: IntRange): Boolean =
        choices.isNotEmpty() && choices.all { it.value in domain && it.label.isNotBlank() } &&
            choices.map { it.value }.distinct().size == choices.size
    if(!valid(modes, 0..1) || !valid(preserveColorChoices, 0..6)) return null
    return RgbCurveOptions(mode, preserveColors, modes.toList(), preserveColorChoices.toList())
        .takeIf { it.accepts(mode, preserveColors) }
}
