/* SPDX-License-Identifier: GPL-3.0-or-later */
package org.example.darktableandroid.nativecore

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class CropAspectTest {
    private val full = CropRect(0f, 0f, 1f, 1f)
    private val minimum = 0.01f

    @Test fun squareUsesInputPixelsRatherThanNormalizedCoordinateAspect() {
        val landscape = requireNotNull(fitCropAspect(full, 6000, 4000, 1.0, minimum))
        assertEquals(1f / 6f, landscape.left, 0.000001f)
        assertEquals(5f / 6f, landscape.right, 0.000001f)
        assertEquals(0f, landscape.top, 0f)
        assertEquals(1f, landscape.bottom, 0f)
        assertPixelAspect(landscape, 6000, 4000, 1.0)
        val portrait = requireNotNull(fitCropAspect(full, 4000, 6000, 1.0, minimum))
        assertEquals(1f / 6f, portrait.top, 0.000001f)
        assertEquals(5f / 6f, portrait.bottom, 0.000001f)
        assertEquals(0f, portrait.left, 0f)
        assertEquals(1f, portrait.right, 0f)
        assertPixelAspect(portrait, 4000, 6000, 1.0)
    }

    @Test fun commonVideoRatioKeepsCenterAndFitsInsideTheDraft() {
        val input = CropRect(0.1f, 0.2f, 0.9f, 0.8f)
        val fitted = requireNotNull(fitCropAspect(input, 1024, 768, 16.0 / 9.0, minimum))
        assertEquals(0.5f, (fitted.left + fitted.right) / 2f, 0.000001f)
        assertEquals(0.5f, (fitted.top + fitted.bottom) / 2f, 0.000001f)
        assertTrue(fitted.left >= input.left && fitted.right <= input.right)
        assertTrue(fitted.top >= input.top && fitted.bottom <= input.bottom)
        assertPixelAspect(fitted, 1024, 768, 16.0 / 9.0)
    }

    @Test fun orientationUsesTheSameReferenceWithoutRepeatedShrinkage() {
        val portrait = requireNotNull(cropPresetAspect("3:2", 6000, 4000, true))
        val landscape = requireNotNull(cropPresetAspect("3:2", 6000, 4000, false))
        assertEquals(2.0 / 3.0, portrait, 0.0)
        assertEquals(1.5, landscape, 0.0)
        assertPixelAspect(requireNotNull(fitCropAspect(full, 6000, 4000, portrait, minimum)), 6000, 4000, portrait)
        assertEquals(full, fitCropAspect(full, 6000, 4000, landscape, minimum))
        assertEquals(1.5, cropPresetAspect("input", 4000, 6000, false)!!, 0.0)
        assertEquals(2.0 / 3.0, cropPresetAspect("input", 4000, 6000, true)!!, 0.0)
    }

    @Test fun inputRatioKeepsTheFullRectangleForNonstandardModuleDimensions() {
        for((width, height) in listOf(6016 to 4016, 1065 to 1793, 479 to 317, 32 to 24,
            1547 to 1547, 1 to Int.MAX_VALUE)) {
            val ratio = requireNotNull(cropPresetAspect("input", width, height, height > width))
            assertEquals("$width × $height", full, fitCropAspect(full, width, height, ratio, minimum))
        }
    }

    @Test fun smallDraftExpandsOnlyEnoughForNativeMinimumAndStaysInFrame() {
        val fitted = requireNotNull(fitCropAspect(CropRect(0.79f, 0.79f, 0.9f, 0.9f),
            2000, 1000, 1.0, 0.1f))
        assertTrue(validCropRect(fitted, 0.1f))
        assertPixelAspect(fitted, 2000, 1000, 1.0)
        val edge = requireNotNull(fitCropAspect(CropRect(0.9f, 0.9f, 1f, 1f), 2000, 1000, 1.0, 0.1f))
        assertTrue(validCropRect(edge, 0.1f))
        assertEquals(1f, edge.bottom, 0f)
        assertPixelAspect(edge, 2000, 1000, 1.0)
    }

    @Test fun absentDimensionsAndImpossibleOrNonfiniteRatiosNeverProduceEdits() {
        for(dimensions in listOf(0 to 4000, 6000 to 0, -1 to 4000)) {
            assertNull(fitCropAspect(full, dimensions.first, dimensions.second, 1.0, minimum))
            assertNull(cropPresetAspect("1:1", dimensions.first, dimensions.second, false))
        }
        for(ratio in listOf(Double.NaN, Double.POSITIVE_INFINITY, 0.0, -1.0, 1e100, 1e-100))
            assertNull(fitCropAspect(full, 6000, 4000, ratio, minimum))
        assertNull(fitCropAspect(CropRect(0.8f, 0f, 0.2f, 1f), 6000, 4000, 1.0, minimum))
        assertNull(cropPresetAspect("unknown", 6000, 4000, false))
    }

    private fun assertPixelAspect(rect: CropRect, width: Int, height: Int, expected: Double) {
        assertEquals(expected, (rect.right - rect.left).toDouble() * width /
            ((rect.bottom - rect.top).toDouble() * height), 0.00001)
    }
}
