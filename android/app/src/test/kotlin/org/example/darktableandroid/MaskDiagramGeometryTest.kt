/* SPDX-License-Identifier: GPL-3.0-or-later */
package org.example.darktableandroid

import org.example.darktableandroid.nativecore.ModuleParameter
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class MaskDiagramGeometryTest {
    private fun field(id: Int, name: String, value: Double, label: String = "circle $id · $name") =
        ModuleParameter("mask.$id.$name", label, "float", value, null, null, emptyList())

    @Test fun nativeStableIdsKeepGeometryAndCloneSourcesWithTheCorrectShape() {
        val parameters = listOf(
            field(15, "radius", 0.1), field(15, "x", 0.25), field(15, "y", 0.5),
            field(15, "source_x", 0.75), field(15, "source_y", 0.5),
            field(27, "radius_x", 0.2), field(27, "radius_y", 0.1),
            field(27, "x", 0.7, "ellipse 27 · center x"), field(27, "y", 0.4), field(27, "rotation", 45.0),
        )
        val shapes = maskDiagramShapes(parameters)
        assertEquals(listOf(15, 27), shapes.map { it.id })
        assertEquals(MaskDiagramKind.CIRCLE, shapes[0].kind)
        assertEquals(0.75f, requireNotNull(shapes[0].sourceX), 0f)
        assertEquals(0.5f, requireNotNull(shapes[0].sourceY), 0f)
        assertEquals("ellipse 27", shapes[1].label)
        assertEquals(MaskDiagramKind.ELLIPSE, shapes[1].kind)
        assertEquals(45f, shapes[1].rotation, 0f)
    }

    @Test fun gradientAnchorIsRecognizedWithoutTreatingActionsOrIncompleteDataAsShapes() {
        val parameters = listOf(
            ModuleParameter("mask.add.circle", "add circle mask", "action", 1.0, 1.0, 1.0, emptyList()),
            field(8, "x", 0.2), field(8, "y", 0.3), field(8, "compression", 0.5), field(8, "rotation", -90.0),
            field(9, "x", 0.2), field(9, "radius", 0.1), // Missing native y.
            field(10, "x", Double.NaN), field(10, "y", 0.5), field(10, "radius", 0.1),
        )
        val shape = maskDiagramShapes(parameters).single()
        assertEquals(8, shape.id)
        assertEquals(MaskDiagramKind.GRADIENT, shape.kind)
        assertEquals(-90f, shape.rotation, 0f)
        assertTrue(maskDiagramShapes(emptyList()).isEmpty())
    }

    private fun node(id: Int, index: Int, x: Double, y: Double, brush: Boolean = false): List<ModuleParameter> =
        listOf(
            field(id, "node.$index.x", x, "${if(brush) "brush" else "path"} $id · node ${index + 1} x"),
            field(id, "node.$index.y", y),
            field(id, "node.$index.ctrl1_x", x - 0.1), field(id, "node.$index.ctrl1_y", y - 0.05),
            field(id, "node.$index.ctrl2_x", x + 0.1), field(id, "node.$index.ctrl2_y", y + 0.05),
            field(id, "node.$index.border_in", 0.02), field(id, "node.$index.border_out", 0.03),
        ) + if(brush) listOf(field(id, "node.$index.hardness", 0.7), field(id, "node.$index.density", 0.8)) else emptyList()

    @Test fun nativeBezierHandlesPreserveClosedPathAndOpenBrushTopology() {
        val parameters = node(40, 2, 0.2, 0.8) + node(40, 0, 0.1, 0.2) + node(40, 1, 0.8, 0.3) +
            node(53, 0, 0.2, 0.4, true) + node(53, 1, 0.7, 0.6, true) +
            listOf(field(53, "source_x", 0.1), field(53, "source_y", 0.9))
        val shapes = maskDiagramShapes(parameters)
        val path = shapes.first { it.id == 40 }
        assertEquals(MaskDiagramKind.PATH, path.kind)
        assertEquals(listOf(0, 1, 2), path.nodes.map { it.index })
        assertEquals(3, maskDiagramSegments(path).size)
        assertEquals(path.nodes.first(), maskDiagramSegments(path).last().end)
        assertEquals(0.2f, maskDiagramSegments(path).first().start.control2X, 0.00001f)
        assertEquals(0.7f, maskDiagramSegments(path).first().end.control1X, 0.00001f)
        val brush = shapes.first { it.id == 53 }
        assertEquals(MaskDiagramKind.BRUSH, brush.kind)
        assertEquals(1, maskDiagramSegments(brush).size)
        assertEquals(brush.nodes.last(), maskDiagramSegments(brush).last().end)
        assertEquals(0.7f, requireNotNull(brush.nodes.first().hardness), 0f)
        assertEquals(0.1f, requireNotNull(brush.sourceX), 0f)
    }

    @Test fun incompleteOrNoncontiguousNodesNeverProduceAnInventedPath() {
        val valid = node(40, 0, 0.1, 0.2) + node(40, 1, 0.8, 0.3) + node(40, 2, 0.2, 0.8)
        assertTrue(maskDiagramShapes(valid.filterNot { it.name.startsWith("mask.40.node.1.") }).isEmpty())
        assertTrue(maskDiagramShapes(valid.filterNot { it.name == "mask.40.node.2.ctrl2_y" }).isEmpty())
        assertTrue(maskDiagramShapes(valid.map {
            if(it.name == "mask.40.node.1.ctrl1_x") it.copy(value = Double.POSITIVE_INFINITY) else it
        }).isEmpty())
        assertTrue(maskDiagramShapes(node(53, 0, 0.2, 0.4, true) +
            node(53, 1, 0.7, 0.6, true).filterNot { it.name.endsWith(".density") }).isEmpty())
        assertNull(maskControlAddress("mask.add.path"))
        assertNull(maskControlAddress("mask.999999999999.node.0.x"))
        assertEquals(MaskControlAddress(40, "node.2.ctrl2_y", 2), maskControlAddress("mask.40.node.2.ctrl2_y"))
    }

    @Test fun rotationUsesTheFullNativeDegreeRange() {
        val ellipse = maskDiagramShapes(listOf(field(27, "radius_x", 0.2), field(27, "radius_y", 0.1),
            field(27, "x", 0.5), field(27, "y", 0.5), field(27, "rotation", 170.0))).single()
        assertEquals(170f, ellipse.rotation, 0f)
    }
}
