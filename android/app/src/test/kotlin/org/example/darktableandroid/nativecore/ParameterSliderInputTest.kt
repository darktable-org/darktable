/* SPDX-License-Identifier: GPL-3.0-or-later */
package org.example.darktableandroid.nativecore

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test

class ParameterSliderInputTest {
    private fun exposure(value: Double = 0.0) = ModuleParameter("exposure", "Exposure", "float", value,
        -18.0, 18.0, emptyList(), sliderMinimum = -3.0, sliderMaximum = 4.0, defaultValue = 0.0)

    @Test fun desktopSoftRangeDoesNotRestrictPreciseInput() {
        val control = exposure()
        assertEquals(-3.0..4.0, control.sliderBounds())
        assertEquals(-18.0, control.parseNumericInput("-18")!!, 0.0)
        assertEquals(18.0, control.parseNumericInput("18")!!, 0.0)
        assertEquals(-3.0..12.0, exposure(12.0).sliderBounds())
        assertEquals(-12.0..4.0, exposure(-12.0).sliderBounds())
    }

    @Test fun missingOrMalformedSoftRangeFallsBackToNativeBounds() {
        assertEquals(-18.0..18.0, exposure().copy(sliderMinimum = null).sliderBounds())
        assertEquals(-18.0..18.0, exposure().copy(sliderMinimum = -100.0).sliderBounds())
        assertEquals(-18.0..18.0, exposure().copy(sliderMaximum = Double.NaN).sliderBounds())
        assertEquals(-18.0..18.0, exposure().copy(sliderMinimum = 4.0).sliderBounds())
    }

    @Test fun sliderNeedsFiniteIncreasingHardBounds() {
        assertNull(exposure().copy(minimum = null).sliderBounds())
        assertNull(exposure().copy(maximum = Double.POSITIVE_INFINITY).sliderBounds())
        assertNull(exposure().copy(maximum = -18.0).sliderBounds())
    }

    @Test fun sliderUsesNativeStoragePrecisionAndIntegerSteps() {
        val control = exposure()
        assertEquals(0.12345679f.toDouble(), control.parseNumericInput(control.sliderInput(0.123456789))!!, 0.0)
        assertEquals("3", control.copy(type = "int").sliderInput(2.75))
        assertEquals("-3", control.copy(type = "int").sliderInput(-2.75))
    }

    @Test fun resetRequiresAValidOriginalDefault() {
        assertEquals(0.0, exposure().resetValue()!!, 0.0)
        assertNull(exposure().copy(defaultValue = null).resetValue())
        assertNull(exposure().copy(defaultValue = Double.NaN).resetValue())
        assertNull(exposure().copy(defaultValue = 19.0).resetValue())
        assertNull(exposure().copy(type = "bool", defaultValue = 2.0).resetValue())
        assertEquals(1.0, exposure().copy(type = "bool", defaultValue = 1.0).resetValue()!!, 0.0)
        val options = listOf(ParameterChoice(2.0, "A"), ParameterChoice(4.0, "B"))
        assertNull(exposure().copy(type = "enum", choices = options, defaultValue = 3.0).resetValue())
        assertEquals(4.0, exposure().copy(type = "enum", choices = options, defaultValue = 4.0).resetValue()!!, 0.0)
    }

    @Test fun optionalSliderAndDefaultMetadataRemainBackwardCompatible() {
        val old = parseModules("""[{"operation":"exposure","name":"Exposure","enabled":true,
            "parameters":[{"name":"exposure","type":"float","value":1,"min":-18,"max":18}]}]""")
            .single().parameters.single()
        assertNull(old.defaultValue)
        assertEquals(-18.0..18.0, old.sliderBounds())
        val updated = parseModules("""[{"operation":"exposure","name":"Exposure","enabled":true,
            "parameters":[{"name":"exposure","type":"float","value":1,"min":-18,"max":18,
            "sliderMin":-3,"sliderMax":4,"default":0}]}]""").single().parameters.single()
        assertEquals(-3.0..4.0, updated.sliderBounds())
        assertEquals(0.0, updated.resetValue()!!, 0.0)
    }
}
