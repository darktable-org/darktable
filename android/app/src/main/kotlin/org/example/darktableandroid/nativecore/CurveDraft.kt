/* SPDX-License-Identifier: GPL-3.0-or-later */
package org.example.darktableandroid.nativecore

data class CurvePoint(val x: Float, val y: Float)

internal fun validCurveNodes(nodes: List<CurvePoint>, maxNodes: Int, minSpacing: Float): Boolean =
    maxNodes in 2..20 && minSpacing.isFinite() && minSpacing > 0f &&
        nodes.size in 2..maxNodes && nodes.all { it.x.isFinite() && it.y.isFinite() && it.x in 0f..1f && it.y in 0f..1f } &&
        nodes.zipWithNext().all { (left, right) -> right.x.toDouble() - left.x.toDouble() >= minSpacing.toDouble() }

internal fun replaceCurvePoint(
    nodes: List<CurvePoint>, index: Int, x: Float, y: Float, maxNodes: Int, minSpacing: Float,
): List<CurvePoint>? {
    if(index !in nodes.indices) return null
    val changed = nodes.toMutableList().apply { this[index] = CurvePoint(x, y) }
    return changed.takeIf { validCurveNodes(it, maxNodes, minSpacing) }
}

/** Constrain a drag in float space, so rounding cannot collapse neighboring nodes. */
internal fun moveCurvePoint(
    nodes: List<CurvePoint>, index: Int, x: Float, y: Float, maxNodes: Int, minSpacing: Float,
): List<CurvePoint>? {
    if(!validCurveNodes(nodes, maxNodes, minSpacing) || index !in nodes.indices || !x.isFinite() || !y.isFinite()) return null
    val lower = if(index > 0) nodes[index - 1].x.toDouble() + minSpacing.toDouble() else 0.0
    val upper = if(index < nodes.lastIndex) nodes[index + 1].x.toDouble() - minSpacing.toDouble() else 1.0
    var lowerFloat = lower.toFloat()
    if(lowerFloat.toDouble() < lower) lowerFloat = Math.nextUp(lowerFloat)
    var upperFloat = upper.toFloat()
    if(upperFloat.toDouble() > upper) upperFloat = Math.nextDown(upperFloat)
    if(lowerFloat > upperFloat) return null
    return replaceCurvePoint(nodes, index, x.coerceIn(lowerFloat, upperFloat), y.coerceIn(0f, 1f), maxNodes, minSpacing)
}

internal data class AddedCurvePoint(val nodes: List<CurvePoint>, val index: Int)

/** Add an editable control point in the widest gap; this does not evaluate the tone curve. */
internal fun addCurvePoint(nodes: List<CurvePoint>, maxNodes: Int, minSpacing: Float): AddedCurvePoint? {
    if(!validCurveNodes(nodes, maxNodes, minSpacing) || nodes.size >= maxNodes) return null
    for(index in (0 until nodes.lastIndex).sortedByDescending { nodes[it + 1].x - nodes[it].x }) {
        val left = nodes[index]
        val right = nodes[index + 1]
        val point = CurvePoint((left.x.toDouble() + right.x.toDouble()).div(2).toFloat(),
            (left.y.toDouble() + right.y.toDouble()).div(2).toFloat())
        val changed = nodes.toMutableList().apply { add(index + 1, point) }
        if(validCurveNodes(changed, maxNodes, minSpacing)) return AddedCurvePoint(changed, index + 1)
    }
    return null
}

internal fun deleteCurvePoint(nodes: List<CurvePoint>, index: Int, maxNodes: Int, minSpacing: Float): List<CurvePoint>? {
    if(index !in nodes.indices || nodes.size <= 2) return null
    return nodes.filterIndexed { k, _ -> k != index }.takeIf { validCurveNodes(it, maxNodes, minSpacing) }
}

internal fun curveCoordinates(nodes: List<CurvePoint>): FloatArray = FloatArray(nodes.size * 2) { index ->
    if(index % 2 == 0) nodes[index / 2].x else nodes[index / 2].y
}
