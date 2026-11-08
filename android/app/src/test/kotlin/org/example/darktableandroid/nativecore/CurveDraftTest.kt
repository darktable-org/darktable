/* SPDX-License-Identifier: GPL-3.0-or-later */
package org.example.darktableandroid.nativecore

import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class CurveDraftTest {
    private val spacing = 0.000001f
    private val identity = listOf(CurvePoint(0f, 0f), CurvePoint(1f, 1f))

    @Test fun draggingClampsAtNeighborsWithoutCollapsingFloatCoordinates() {
        val original = listOf(CurvePoint(0f, 0f), CurvePoint(0.5f, 0.4f), CurvePoint(1f, 1f))
        val towardRight = moveCurvePoint(original, 1, 2f, 3f, 20, spacing)!!
        assertTrue(validCurveNodes(towardRight, 20, spacing))
        assertTrue(towardRight[2].x.toDouble() - towardRight[1].x.toDouble() >= spacing.toDouble())
        assertEquals(1f, towardRight[1].y, 0f)
        val towardLeft = moveCurvePoint(original, 1, -2f, -3f, 20, spacing)!!
        assertTrue(validCurveNodes(towardLeft, 20, spacing))
        assertTrue(towardLeft[1].x.toDouble() - towardLeft[0].x.toDouble() >= spacing.toDouble())
        assertEquals(0f, towardLeft[1].y, 0f)
        assertEquals(CurvePoint(0.5f, 0.4f), original[1])
        assertNull(moveCurvePoint(original, 1, Float.NaN, 0f, 20, spacing))
        assertNull(moveCurvePoint(original, 1, 0f, Float.POSITIVE_INFINITY, 20, spacing))
        assertNull(moveCurvePoint(original, -1, 0f, 0f, 20, spacing))
    }

    @Test fun numericEditsRejectInvalidValuesWithoutChangingTheDraft() {
        val original = listOf(CurvePoint(0f, 0f), CurvePoint(0.5f, 0.4f), CurvePoint(1f, 1f))
        assertNull(replaceCurvePoint(original, 1, 1f, 0.5f, 20, spacing))
        assertNull(replaceCurvePoint(original, 1, -0.1f, 0.5f, 20, spacing))
        assertNull(replaceCurvePoint(original, 1, 0.5f, 1.1f, 20, spacing))
        assertNull(replaceCurvePoint(original, 1, Float.NaN, 0.5f, 20, spacing))
        val edited = replaceCurvePoint(original, 1, 0.25f, 0.75f, 20, spacing)!!
        assertEquals(listOf(CurvePoint(0f, 0f), CurvePoint(0.25f, 0.75f), CurvePoint(1f, 1f)), edited)
        assertEquals(CurvePoint(0.5f, 0.4f), original[1])
    }

    @Test fun validationChecksCapacityRangeSpacingAndFiniteFloatCoordinates() {
        assertTrue(validCurveNodes(identity, 20, spacing))
        assertFalse(validCurveNodes(identity.take(1), 20, spacing))
        assertFalse(validCurveNodes(List(21) { CurvePoint(it / 20f, it / 20f) }, 20, spacing))
        assertFalse(validCurveNodes(identity, 1, spacing))
        assertFalse(validCurveNodes(identity, 21, spacing))
        assertFalse(validCurveNodes(identity, 20, 0f))
        assertFalse(validCurveNodes(identity, 20, Float.NaN))
        assertFalse(validCurveNodes(listOf(CurvePoint(Float.NaN, 0f), CurvePoint(1f, 1f)), 20, spacing))
        assertFalse(validCurveNodes(listOf(CurvePoint(0f, Float.POSITIVE_INFINITY), CurvePoint(1f, 1f)), 20, spacing))
        assertFalse(validCurveNodes(listOf(CurvePoint(0f, 0f), CurvePoint(1.1f, 1f)), 20, spacing))
        assertFalse(validCurveNodes(identity.reversed(), 20, spacing))
        // Distinct numeric input can round to the same float before JNI sees it.
        assertFalse(validCurveNodes(listOf(CurvePoint(0.5f, 0f), CurvePoint((0.5 + 1e-8).toFloat(), 1f)), 20, spacing))
    }

    @Test fun addingUsesTheWidestGapAndPreservesExistingControlPoints() {
        val original = listOf(CurvePoint(0f, 0f), CurvePoint(0.1f, 0.2f), CurvePoint(1f, 1f))
        val added = addCurvePoint(original, 20, spacing)!!
        assertEquals(2, added.index)
        assertTrue(validCurveNodes(added.nodes, 20, spacing))
        assertEquals(0.55f, added.nodes[2].x, 0.000001f)
        assertEquals(0.6f, added.nodes[2].y, 0.000001f)
        assertEquals(original, added.nodes.filterIndexed { index, _ -> index != added.index })
        assertEquals(3, original.size)
    }

    @Test fun addingHonorsTheNodeLimitAndAvailableFloatSpacing() {
        assertNull(addCurvePoint(identity, 2, spacing))
        // The existing two nodes fit, but neither half of the gap fits a third.
        assertTrue(validCurveNodes(identity, 20, 0.6f))
        assertNull(addCurvePoint(identity, 20, 0.6f))
        val full = List(20) { CurvePoint(it / 19f, it / 19f) }
        assertTrue(validCurveNodes(full, 20, spacing))
        assertNull(addCurvePoint(full, 20, spacing))
        val almostFull = List(19) { CurvePoint(it / 18f, it / 18f) }
        val last = addCurvePoint(almostFull, 20, spacing)
        assertNotNull(last)
        assertEquals(20, last!!.nodes.size)
        assertTrue(validCurveNodes(last.nodes, 20, spacing))
    }

    @Test fun deletingKeepsAtLeastTwoNodesAndRejectsInvalidSelections() {
        assertNull(deleteCurvePoint(identity, 0, 20, spacing))
        val original = listOf(CurvePoint(0f, 0f), CurvePoint(0.5f, 0.4f), CurvePoint(1f, 1f))
        assertEquals(identity, deleteCurvePoint(original, 1, 20, spacing))
        assertNull(deleteCurvePoint(original, -1, 20, spacing))
        assertNull(deleteCurvePoint(original, original.size, 20, spacing))
        assertEquals(3, original.size)
    }

    @Test fun coordinatesKeepNodeOrderAndInterleaveInputOutputPairsForJni() {
        val original = listOf(CurvePoint(0f, 0.75f), CurvePoint(0.25f, 0.5f), CurvePoint(1f, 1f))
        assertArrayEquals(floatArrayOf(0f, 0.75f, 0.25f, 0.5f, 1f, 1f), curveCoordinates(original), 0f)
    }
}
