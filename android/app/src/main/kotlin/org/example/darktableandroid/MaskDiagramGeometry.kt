/* SPDX-License-Identifier: GPL-3.0-or-later */
package org.example.darktableandroid

import org.example.darktableandroid.nativecore.ModuleParameter

internal enum class MaskDiagramKind { CIRCLE, ELLIPSE, GRADIENT, PATH, BRUSH }

internal data class MaskDiagramNode(
    val index: Int, val x: Float, val y: Float,
    val control1X: Float, val control1Y: Float, val control2X: Float, val control2Y: Float,
    val borderIn: Float, val borderOut: Float,
    val hardness: Float? = null, val density: Float? = null,
)

internal data class MaskDiagramShape(
    val id: Int, val label: String, val kind: MaskDiagramKind,
    val x: Float, val y: Float, val radiusX: Float, val radiusY: Float,
    val rotation: Float, val sourceX: Float?, val sourceY: Float?, val enabled: Boolean,
    val nodes: List<MaskDiagramNode> = emptyList(),
)

internal data class MaskDiagramSegment(val start: MaskDiagramNode, val end: MaskDiagramNode)
internal data class MaskControlAddress(val formId: Int, val field: String, val nodeIndex: Int?)

private val maskShapePath = Regex("^mask\\.([1-9][0-9]*)\\.(.+)$")
private val maskNodePath = Regex("^node\\.(0|[1-9][0-9]*)\\.(.+)$")

internal fun maskControlAddress(name: String): MaskControlAddress? {
    val form = maskShapePath.matchEntire(name) ?: return null
    val id = form.groupValues[1].toIntOrNull() ?: return null
    val node = maskNodePath.matchEntire(form.groupValues[2])
    return MaskControlAddress(id, form.groupValues[2], node?.groupValues?.get(1)?.toIntOrNull())
}

internal fun maskControlLabel(id: Int, parameters: List<ModuleParameter>): String =
    parameters.firstOrNull()?.label?.substringBefore('·')?.trim()?.takeIf { it.isNotEmpty() } ?: "mask $id"

/** Segments use the original outgoing/incoming handles, and only paths close. */
internal fun maskDiagramSegments(shape: MaskDiagramShape): List<MaskDiagramSegment> {
    if(shape.kind != MaskDiagramKind.PATH && shape.kind != MaskDiagramKind.BRUSH) return emptyList()
    val adjacent = shape.nodes.zipWithNext { start, end -> MaskDiagramSegment(start, end) }
    return if(shape.kind == MaskDiagramKind.PATH && shape.nodes.size >= 3)
        adjacent + MaskDiagramSegment(shape.nodes.last(), shape.nodes.first()) else adjacent
}

/** Read native coordinates without joining incomplete or noncontiguous topology. */
internal fun maskDiagramShapes(parameters: List<ModuleParameter>): List<MaskDiagramShape> {
    val groups = linkedMapOf<Int, MutableMap<String, ModuleParameter>>()
    parameters.forEach { parameter ->
        val address = maskControlAddress(parameter.name) ?: return@forEach
        groups.getOrPut(address.formId) { linkedMapOf() }[address.field] = parameter
    }
    return groups.mapNotNull { (id, fields) ->
        fun number(name: String): Float? = fields[name]?.value?.toFloat()?.takeIf { it.isFinite() }
        fun coordinate(name: String): Float? = number(name)?.takeIf { it in -100f..100f }
        val nodeFields = fields.keys.mapNotNull { maskNodePath.matchEntire(it)?.groupValues?.get(1)?.toIntOrNull() }
            .distinct().sorted()
        if(nodeFields.isNotEmpty()) {
            if(nodeFields.size !in 2..64 || nodeFields != (0 until nodeFields.size).toList()) return@mapNotNull null
            val brush = fields.keys.any { it.endsWith(".hardness") || it.endsWith(".density") }
            if(!brush && nodeFields.size < 3) return@mapNotNull null
            val nodes = nodeFields.map { index ->
                val prefix = "node.$index."
                val x = coordinate(prefix + "x") ?: return@mapNotNull null
                val y = coordinate(prefix + "y") ?: return@mapNotNull null
                val c1x = coordinate(prefix + "ctrl1_x") ?: return@mapNotNull null
                val c1y = coordinate(prefix + "ctrl1_y") ?: return@mapNotNull null
                val c2x = coordinate(prefix + "ctrl2_x") ?: return@mapNotNull null
                val c2y = coordinate(prefix + "ctrl2_y") ?: return@mapNotNull null
                val borderIn = coordinate(prefix + "border_in")?.takeIf { it >= 0f } ?: return@mapNotNull null
                val borderOut = coordinate(prefix + "border_out")?.takeIf { it >= 0f } ?: return@mapNotNull null
                val hardness = number(prefix + "hardness")?.takeIf { it in 0f..1f }
                val density = number(prefix + "density")?.takeIf { it in 0f..1f }
                if(brush && (hardness == null || density == null)) return@mapNotNull null
                MaskDiagramNode(index, x, y, c1x, c1y, c2x, c2y, borderIn, borderOut, hardness, density)
            }
            val first = nodes.first()
            return@mapNotNull MaskDiagramShape(id, maskControlLabel(id, fields.values.toList()),
                if(brush) MaskDiagramKind.BRUSH else MaskDiagramKind.PATH,
                first.x, first.y, 0f, 0f, 0f, coordinate("source_x"), coordinate("source_y"),
                fields["enabled"]?.value != 0.0, nodes)
        }
        val x = coordinate("x") ?: return@mapNotNull null
        val y = coordinate("y") ?: return@mapNotNull null
        val radius = coordinate("radius")
        val radiusX = coordinate("radius_x")
        val radiusY = coordinate("radius_y")
        val kind = when {
            radius != null && radius > 0f -> MaskDiagramKind.CIRCLE
            radiusX != null && radiusX > 0f && radiusY != null && radiusY > 0f -> MaskDiagramKind.ELLIPSE
            number("compression") != null -> MaskDiagramKind.GRADIENT
            else -> return@mapNotNull null
        }
        MaskDiagramShape(id, fields.getValue("x").label.substringBefore('·').trim(), kind,
            x, y, radius ?: radiusX ?: 0f, radius ?: radiusY ?: 0f,
            number("rotation") ?: 0f, coordinate("source_x"), coordinate("source_y"),
            fields["enabled"]?.value != 0.0)
    }
}
