/* SPDX-License-Identifier: GPL-3.0-or-later */
package org.example.darktableandroid

import org.example.darktableandroid.nativecore.ModuleParameter

/** The four thresholds address the original blend channel, not a display index. */
internal data class ParametricMaskChannel(
    val index: Int,
    val thresholds: List<ModuleParameter>,
    val controls: List<ModuleParameter>,
) {
    val output: Boolean get() = index and 4 != 0
    val label: String get() = thresholds.first().label.substringBefore('·').trim()
    val parameters: List<ModuleParameter> get() = thresholds + controls
    val enabled: Boolean get() = (controls.firstOrNull { it.name.endsWith(".enabled") }?.value ?: 1.0) != 0.0
    val inverted: Boolean get() = controls.firstOrNull { it.name.endsWith(".inverted") }?.value == 1.0

    /** Neighbor limits prevent an individual native edit from crossing another handle. */
    fun boundedThreshold(handle: Int): ModuleParameter? {
        if(handle !in 0..3 || thresholds.size != 4) return null
        val values = thresholds.map { it.value }
        if(values.any { !it.isFinite() || it !in 0.0..1.0 } || values.zipWithNext().any { it.first > it.second })
            return null
        return thresholds[handle].copy(
            minimum = if(handle == 0) 0.0 else values[handle - 1],
            maximum = if(handle == 3) 1.0 else values[handle + 1],
            sliderMinimum = null,
            sliderMaximum = null,
        )
    }
}

internal data class ParametricMaskControls(
    val global: List<ModuleParameter>,
    val channels: List<ParametricMaskChannel>,
) {
    val names: Set<String> get() = (global + channels.flatMap { it.parameters }).map { it.name }.toSet()
    val isEmpty: Boolean get() = global.isEmpty() && channels.isEmpty()
}

/** Unknown or incomplete optional metadata stays with the generic parameter renderer. */
internal fun parametricMaskControls(parameters: List<ModuleParameter>): ParametricMaskControls {
    val globalNames = listOf("blend.parametric.enabled", "blend.parametric.combine", "blend.parametric.reset")
    val global = globalNames.mapNotNull { name -> parameters.singleOrNull { it.name == name } }
    val pattern = Regex("blend\\.parametric\\.([0-9]{1,2})\\.(p[0-3]|enabled|inverted|boost_ev|reset)")
    val groups = parameters.mapNotNull { parameter ->
        val match = pattern.matchEntire(parameter.name) ?: return@mapNotNull null
        val channel = match.groupValues[1].toInt()
        if(channel !in setOf(0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 12, 13, 14)) return@mapNotNull null
        Triple(channel, match.groupValues[2], parameter)
    }.groupBy { it.first }
    val channels = groups.toSortedMap().mapNotNull { (channel, fields) ->
        if(fields.map { it.second }.distinct().size != fields.size) return@mapNotNull null
        val thresholds = (0..3).map { handle -> fields.singleOrNull { it.second == "p$handle" }?.third }
        if(thresholds.any { it == null }) return@mapNotNull null
        val complete = thresholds.filterNotNull()
        if(complete.any { it.type != "float" || it.minimum != 0.0 || it.maximum != 1.0 }) return@mapNotNull null
        val controls = listOf("enabled", "inverted", "boost_ev", "reset").mapNotNull { name ->
            fields.singleOrNull { it.second == name }?.third
        }
        ParametricMaskChannel(channel, complete, controls).takeIf { it.boundedThreshold(0) != null }
    }
    return ParametricMaskControls(global, channels)
}

/** Threshold-layout schematic only; the original pixelpipe evaluates the actual mask. */
internal fun parametricThresholdDiagramValue(channel: ParametricMaskChannel, position: Double): Double? {
    if(!position.isFinite() || position !in 0.0..1.0 || channel.boundedThreshold(0) == null) return null
    if(!channel.enabled) return 1.0
    val points = channel.thresholds.map { it.value }
    val rising = when {
        position >= points[1] -> 1.0
        position <= points[0] -> 0.0
        else -> (position - points[0]) / (points[1] - points[0])
    }
    val falling = when {
        position <= points[2] -> 1.0
        position >= points[3] -> 0.0
        else -> (points[3] - position) / (points[3] - points[2])
    }
    val selected = minOf(rising, falling)
    return if(channel.inverted) 1.0 - selected else selected
}
