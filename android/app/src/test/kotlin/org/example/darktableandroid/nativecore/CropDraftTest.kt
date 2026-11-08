/* SPDX-License-Identifier: GPL-3.0-or-later */
package org.example.darktableandroid.nativecore

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class CropDraftTest {
    private val minimum = 0.01f

    @Test fun validRectanglesKeepTheSameFloatCoordinatesSentToNative() {
        assertEquals(CropRect(0f, 0f, 1f, 1f), parseCropInput("0", "0", "1", "1", minimum))
        assertEquals(CropRect(0.1f, 0.2f, 0.8f, 0.9f), parseCropInput("0.1", "0.2", "0.8", "0.9", minimum))
        assertTrue(validCropRect(CropRect(0f, 0f, minimum, minimum), minimum))
    }

    @Test fun invalidNumbersAndOutOfRangeBoundariesCannotBecomeEdits() {
        for(input in listOf("", "invalid", "NaN", "Infinity", "-Infinity", "1e100", "-0.1", "1.1")) {
            assertNull(input, parseCropInput(input, "0", "1", "1", minimum))
        }
        assertNull(parseCropInput("0", "-0.1", "1", "1", minimum))
        assertNull(parseCropInput("0", "0", "1.1", "1", minimum))
        assertNull(parseCropInput("0", "0", "1", "NaN", minimum))
    }

    @Test fun reversedCollapsedAndTooSmallAreasAreRejectedBeforeJni() {
        assertNull(parseCropInput("0.8", "0", "0.2", "1", minimum))
        assertNull(parseCropInput("0", "0.8", "1", "0.2", minimum))
        assertNull(parseCropInput("0.5", "0", "0.5", "1", minimum))
        assertNull(parseCropInput("0", "0", "0.009", "1", minimum))
        // Distinct decimal coordinates can collapse to the same native Float.
        assertNull(parseCropInput("0.5", "0", "0.500000001", "1", minimum))
    }

    @Test fun minimumSizeMustBeFinitePositiveAndNormalized() {
        val full = CropRect(0f, 0f, 1f, 1f)
        for(minimum in listOf(Float.NaN, Float.POSITIVE_INFINITY, 0f, -0.01f, 1.01f)) {
            assertFalse(validCropRect(full, minimum))
        }
        assertTrue(validCropRect(full, 1f))
        // Bad metadata must not permit zero area if the minimum rounds away.
        assertFalse(validCropRect(CropRect(0.5f, 0.5f, 0.5f, 0.5f), Float.MIN_VALUE))
    }

    @Test fun minimumEdgesUseDesktopFloatAdditionAtInteriorCoordinates() {
        val minimumEdge = 0.5f + minimum
        assertEquals(CropRect(0.5f, 0.5f, 0.51f, 0.51f), parseCropInput("0.5", "0.5", "0.51", "0.51", minimum))
        assertFalse(validCropRect(CropRect(0.5f, 0.5f, Math.nextDown(minimumEdge), minimumEdge), minimum))
        assertFalse(validCropRect(CropRect(0.5f, 0.5f, minimumEdge, Math.nextDown(minimumEdge)), minimum))
    }
}
