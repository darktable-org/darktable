/* SPDX-License-Identifier: GPL-3.0-or-later */
package org.example.darktableandroid.nativecore

import org.junit.Assert.assertEquals
import org.junit.Test

class CropInputMetadataTest {
    @Test fun validNativeInputDimensionsArePreserved() {
        val crop = readCrop("6000", "4000")
        assertEquals(6000, crop.inputWidth)
        assertEquals(4000, crop.inputHeight)
        assertEquals(CropRect(0f, 0f, 1f, 1f), crop.bounds)
    }

    @Test fun legacyAndInvalidOptionalDimensionsPreserveBoundsWithoutEnablingRatios() {
        val legacy = requireNotNull(parseModules("""[{"operation":"crop","name":"crop","enabled":true,
          "crop":{"left":0,"top":0,"right":1,"bottom":1,"minSize":0.01,"freehand":true}}]""").single().crop)
        assertEquals(0, legacy.inputWidth)
        assertEquals(0, legacy.inputHeight)
        for(value in listOf("0", "-1", "1.5", "2147483648", "null", "true", "\"6000\"")) {
            val width = readCrop(value, "4000")
            val height = readCrop("6000", value)
            assertEquals(legacy, width)
            assertEquals(legacy, height)
        }
    }

    private fun readCrop(width: String, height: String) = requireNotNull(parseModules("""[{
      "operation":"crop","name":"crop","enabled":true,
      "crop":{"left":0,"top":0,"right":1,"bottom":1,"minSize":0.01,"freehand":true,
        "inputWidth":$width,"inputHeight":$height}}]""").single().crop)
}
