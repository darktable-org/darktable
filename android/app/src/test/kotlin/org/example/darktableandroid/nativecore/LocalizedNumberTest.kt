/* SPDX-License-Identifier: GPL-3.0-or-later */
package org.example.darktableandroid.nativecore

import java.util.Locale
import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test

class LocalizedNumberTest {
    @Test fun dotAndCommaKeepTheSameSignedAndScientificValues() {
        listOf("0,125" to "0.125", " -12,5 " to "-12.5", "+,5" to "+.5",
            "-2,25e-3" to "-2.25e-3", "1,0E+2" to "1.0E+2").forEach { (localized, canonical) ->
            assertEquals(canonical.toDouble(), localized.toEditorDoubleOrNull()!!, 0.0)
            assertEquals(canonical.toFloat(), localized.toEditorFloatOrNull()!!, 0f)
        }
        assertEquals((-0.0).toRawBits(), "-0,0".toEditorDoubleOrNull()!!.toRawBits())
    }

    @Test fun SpanishFormattedDecimalsWorkWithoutChangingProcessLocale() {
        val original = Locale.getDefault()
        listOf("es-AR", "es-419", "zh-Hans", "zh-Hant").forEach { tag ->
            val input = String.format(Locale.forLanguageTag(tag), "%.3f", 1.25)
            assertEquals(tag, 1.25, input.toEditorDoubleOrNull()!!, 0.0)
        }
        assertEquals(original, Locale.getDefault())
    }

    @Test fun groupingMixedSeparatorsAndNonfiniteValuesCannotBecomeEdits() {
        listOf("", " ", "1,234.5", "1.234,5", "1,2,3", "1.2.3", "1 234", "1_234",
            "1e1,5", ",", ".", "NaN", "Infinity", "-Infinity", "1e999").forEach { input ->
            assertNull(input, input.toEditorDoubleOrNull())
            assertNull(input, input.toEditorFloatOrNull())
        }
        assertEquals(1e100, "1e100".toEditorDoubleOrNull()!!, 0.0)
        assertNull("1e100".toEditorFloatOrNull())
    }

    @Test fun localizedParametersStillCheckNativeFloatBoundsAndIntegralStorage() {
        val minimum = .1f.toDouble()
        val maximum = .2f.toDouble()
        val float = ModuleParameter("amount", "Amount", "float", minimum, minimum, maximum, emptyList())
        assertEquals(minimum, float.parseNumericInput("0,1")!!, 0.0)
        assertEquals(maximum, float.parseNumericInput("0,2")!!, 0.0)
        assertNull(float.parseNumericInput("0,099"))
        assertNull(float.parseNumericInput("0,201"))
        val integer = float.copy(type = "int", minimum = -2.0, maximum = 2.0)
        assertEquals(2.0, integer.parseNumericInput("2,0")!!, 0.0)
        assertNull(integer.parseNumericInput("0,5"))
        assertNull(integer.parseNumericInput("3,0"))
    }

    @Test fun cropBatchesKeepNativeFloatCoordinatesAndRejectInvalidGeometry() {
        assertEquals(parseCropInput("0.1", "0.2", "0.8", "0.9", .01f),
            parseCropInput("0,1", "0,2", "0,8", "0,9", .01f))
        assertNull(parseCropInput("0,8", "0,2", "0,1", "0,9", .01f))
        assertNull(parseCropInput("0", "0", "0,009", "1", .01f))
        assertNull(parseCropInput("0,1.2", "0", "1", "1", .01f))
    }

    @Test fun whiteBalanceGainsKeepOriginalCoefficientIndexesAndBounds() {
        val channels = listOf(WhiteBalanceChannel(2, "cyan", .7f), WhiteBalanceChannel(3, "yellow", 1.3f),
            WhiteBalanceChannel(1, "magenta", 1.1f), WhiteBalanceChannel(0, "green", 1.8f))
        val balance = checkedWhiteBalance(channels, .001f, 8f, 0, "as shot",
            listOf(WhiteBalanceSetting(0, "as shot")))!!
        assertArrayEquals(floatArrayOf(1.8f, 1.1f, .7f, 1.3f),
            whiteBalanceGains(balance, listOf("0,7", "1,3", "1,1", "1,8")), 0f)
        assertNull(whiteBalanceGains(balance, listOf("0,7", "8,000001", "1,1", "1,8")))
    }

    @Test fun whiteBalanceTemperaturePairsKeepPrecisionAndRejectAnInvalidSide() {
        val temperature = checkedWhiteBalanceTemperature(6504.125, 1.000002,
            1901.0, 25000.0, .135, 2.326)!!
        assertEquals(6504.125 to 1.000002, whiteBalanceTemperature(temperature, "6504,125", "1,000002"))
        assertNull(whiteBalanceTemperature(temperature, "1900,9999", "1,0"))
        assertNull(whiteBalanceTemperature(temperature, "6504,125", "2,3260001"))
        assertNull(whiteBalanceTemperature(temperature, "6,504.125", "1,0"))
    }
}
