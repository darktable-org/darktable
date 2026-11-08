/* SPDX-License-Identifier: GPL-3.0-or-later */
package org.example.darktableandroid.nativecore

import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class ExportSettingsTest {
    private fun options(
        pixels: List<Int> = listOf(0, 2),
        profiles: List<Int> = listOf(0, 1),
        intents: List<Int> = listOf(0, 2),
        png: ExportNumberOption? = ExportNumberOption(-1, 0, 9, 5),
        tiff: ExportChoiceOption? = null,
        level: ExportNumberOption? = null,
        dpi: ExportNumberOption? = null,
    ) = ExportOptions(
        ExportNumberOption(0, 0, Int.MAX_VALUE), ExportNumberOption(0, 0, Int.MAX_VALUE),
        pixels.map { ExportOptionChoice(it, "pixel $it") }, 0,
        profiles.map { ExportOptionChoice(it, "profile $it") }, 0,
        intents, 0, listOf(ExportOptionChoice(0, "default"), ExportOptionChoice(1, "omit source")), 0,
        pngCompression = png, tiffCompression = tiff, tiffCompressionLevel = level, dpi = dpi,
    )

    @Test fun packedRequestHasNativeFieldOrderAndNoMutableArrayAlias() {
        val settings = ExportSettings(640, 480, 87, 2, 4, 2, 9, 3, 600, 7, 4, 1)
        val packed = settings.toNativeArray()
        assertArrayEquals(intArrayOf(640, 480, 87, 2, 4, 2, 9, 3, 600, 7, 4, 1), packed)
        val captured = ExportSettings.fromNativeArray(packed)!!
        packed[0] = 1
        assertEquals(settings, captured)
        assertEquals(640, settings.toNativeArray()[0])
        assertEquals(640, captured.maxWidth)
    }

    @Test fun unpackingRejectsMissingFieldsAndInvalidNativeValues() {
        val valid = ExportSettings().toNativeArray()
        assertNull(ExportSettings.fromNativeArray(valid.copyOf(11)))
        assertNull(ExportSettings.fromNativeArray(valid.copyOf(13)))
        val invalid = listOf(0 to -1, 1 to -1, 2 to 4, 2 to 101, 3 to 4, 4 to 10,
            5 to 3, 6 to 0, 6 to 10, 7 to 5, 8 to 71, 8 to 9601, 9 to 8, 10 to 5, 11 to 2)
        for((index, value) in invalid) {
            assertNull("field $index, value $value", ExportSettings.fromNativeArray(valid.copyOf().also { it[index] = value }))
        }
        assertEquals(Int.MAX_VALUE, ExportSettings.fromNativeArray(valid.copyOf().also { it[0] = Int.MAX_VALUE })!!.maxWidth)
    }

    @Test fun switchingFormatClearsUnsupportedSettingsAndUsesOnlyNativeChoices() {
        val png = options()
        val previous = ExportSettings(640, 480, 87, 3, 4, 2, 9, 3, 600, 7, 4, 1)
        assertFalse(png.isValid(previous))
        val converted = png.compatibleSettings(previous)
        assertEquals(ExportSettings(640, 480, 87, 0, 4, -1, -1, -1, 0, 0, 0, 1), converted)
        assertTrue(png.isValid(converted))
        // A format's name must not imply capabilities omitted by its metadata.
        assertFalse(png.isValid(converted.copy(pixelFormat = 1)))
        assertFalse(png.isValid(converted.copy(outputProfile = 7)))
        assertFalse(png.isValid(converted.copy(renderingIntent = 1)))
        assertTrue(png.isValid(converted.copy(pixelFormat = 2, outputProfile = 1, renderingIntent = 2)))
    }

    @Test fun inheritedSentinelsAreValidOutsideNumericRanges() {
        val tiff = options(pixels = listOf(0, 1, 2, 3), png = null,
            tiff = ExportChoiceOption(-1, listOf(-1, 1), 1),
            level = ExportNumberOption(-1, 1, 9, 6), dpi = ExportNumberOption(0, 72, 9600, 300))
        val inherited = tiff.defaultSettings()
        assertEquals(-1, inherited.tiffCompressionLevel)
        assertEquals(0, inherited.dpi)
        assertTrue(tiff.isValid(inherited))
        assertFalse(tiff.isValid(inherited.copy(tiffCompressionLevel = 0)))
        assertFalse(tiff.isValid(inherited.copy(dpi = 71)))
        assertFalse(tiff.isValid(inherited.copy(tiffCompression = 0)))
        assertTrue(tiff.isValid(inherited.copy(tiffCompression = 1, tiffCompressionLevel = 1, dpi = 72)))
        assertTrue(tiff.isValid(inherited.copy(tiffCompressionLevel = 9, dpi = 9600)))
    }

    @Test fun defaultsFollowAdvertisedValuesAndBounds() {
        val native = options().copy(
            maxWidth = ExportNumberOption(320, 0, 640), pixelFormatDefault = 2,
            profileDefault = 1, intentDefault = 2, metadataDefault = 1,
            quality = ExportNumberOption(80, 10, 90),
        )
        val defaults = native.defaultSettings()
        assertEquals(320, defaults.maxWidth)
        assertEquals(80, defaults.quality)
        assertEquals(2, defaults.pixelFormat)
        assertEquals(1, defaults.outputProfile)
        assertEquals(2, defaults.renderingIntent)
        assertEquals(1, defaults.metadataPolicy)
        assertTrue(native.isValid(defaults))
        assertEquals(85, native.defaultSettings(85).quality)
        assertFalse(native.isValid(defaults.copy(maxWidth = 641)))
        assertEquals(320, native.compatibleSettings(defaults.copy(maxWidth = 641)).maxWidth)
    }

    @Test fun advertisedQualityDefaultDoesNotDependOnRangeClamping() {
        val native = options().copy(quality = ExportNumberOption(80, 5, 100))
        assertEquals(80, native.defaultSettings().quality)
        assertEquals(95, native.defaultSettings(95).quality)
    }
}
