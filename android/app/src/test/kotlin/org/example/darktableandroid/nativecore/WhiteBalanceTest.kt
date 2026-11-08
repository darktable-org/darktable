/* SPDX-License-Identifier: GPL-3.0-or-later */
package org.example.darktableandroid.nativecore

import org.junit.Assert.*
import org.junit.Test

class WhiteBalanceTest {
    private val rgb = listOf(WhiteBalanceChannel(0, "red", 2f), WhiteBalanceChannel(1, "green", 0.8f),
        WhiteBalanceChannel(2, "blue", 1.5f))
    private val settings = listOf(WhiteBalanceSetting(0, "as shot"), WhiteBalanceSetting(4, "as shot to reference"))
    private fun balance(channels: List<WhiteBalanceChannel> = rgb) =
        checkedWhiteBalance(channels, 0.001f, 8f, 4, "as shot to reference", settings)!!

    @Test fun channelGainsRetainIndependentGreenWithoutNormalization() {
        assertArrayEquals(floatArrayOf(2f, 0.8f, 1.5f), whiteBalanceGains(balance(), listOf("2", "0.8", "1.5")), 0f)
        assertTrue(balance().acceptsSetting(0))
        assertFalse(balance().acceptsSetting(3)) // A missing camera-reference setting stays unavailable.
        assertFalse(balance().acceptsSetting(2)) // Manual is selected through the coefficient operation.
    }

    @Test fun cygmDisplayOrderMapsToOriginalNativeCoefficientIndexes() {
        val channels = listOf(WhiteBalanceChannel(2, "cyan", 0.7f), WhiteBalanceChannel(3, "yellow", 1.3f),
            WhiteBalanceChannel(1, "magenta", 1.1f), WhiteBalanceChannel(0, "green", 1.8f))
        assertArrayEquals(floatArrayOf(1.8f, 1.1f, 0.7f, 1.3f),
            whiteBalanceGains(balance(channels), listOf("0.7", "1.3", "1.1", "1.8")), 0f)
    }

    @Test fun invalidBatchDoesNotRoundClampOrPartiallyProduceCoefficients() {
        assertArrayEquals(floatArrayOf(0.001f, 8f, 1f), whiteBalanceGains(balance(), listOf("0.001", "8", "1")), 0f)
        for(invalid in listOf("0", "-1", "8.000001", "NaN", "Infinity", "1e999", ""))
            assertNull(whiteBalanceGains(balance(), listOf("1", invalid, "1")))
        assertNull(whiteBalanceGains(balance(), listOf("1", "1")))
        assertFalse(balance().acceptsGains(floatArrayOf(1f, 1f, 1f, 1f)))
    }

    @Test fun originalPresetCoefficientsCanExceedTheManualRangeWithoutClipping() {
        val channels = rgb.map { if(it.index == 2) it.copy(value = 8.094203f) else it }
        val original = balance(channels)
        assertEquals(8.094203f, original.channels[2].value, 0f)
        assertNull(whiteBalanceGains(original, listOf("2", "0.8", "8.094203")))
        assertTrue(original.acceptsSetting(4))
        assertEquals(8f, original.manualMax, 0f)
    }

    @Test fun incompleteOrNonfiniteNativeMetadataCannotCreateAnEditor() {
        assertNull(checkedWhiteBalance(rgb.take(2), 0.001f, 8f, 2, "user", settings))
        assertNull(checkedWhiteBalance(rgb.map { it.copy(index = 0) }, 0.001f, 8f, 2, "user", settings))
        assertNull(checkedWhiteBalance(rgb.map { it.copy(value = Float.NaN) }, 0.001f, 8f, 2, "user", settings))
        assertNull(checkedWhiteBalance(rgb, 0f, 8f, 2, "user", settings))
        assertNull(checkedWhiteBalance(rgb, 0.001f, 8f, 2, "user", settings + WhiteBalanceSetting(2, "manual")))
    }

    private fun temperature() = checkedWhiteBalanceTemperature(6504.125, 1.000002,
        1901.0, 25000.0, 0.135, 2.326)!!

    @Test fun temperatureAndTintAreForwardedTogetherWithoutRoundingOrCoefficientConversion() {
        val temperature = temperature()
        assertEquals(6504.125 to 1.000002, whiteBalanceTemperature(temperature, "6504.125", "1.000002"))
        assertEquals(1901.0 to 2.326, whiteBalanceTemperature(temperature, "1901", "2.326"))
        assertEquals(25000.0 to 0.135, whiteBalanceTemperature(temperature, "25000", "0.135"))
    }

    @Test fun temperatureRejectsTheWholePairWhenEitherValueIsInvalid() {
        val temperature = temperature()
        for(invalid in listOf("", "NaN", "Infinity", "1e999", "1900.9999", "25000.0001"))
            assertNull(whiteBalanceTemperature(temperature, invalid, "1"))
        for(invalid in listOf("", "NaN", "Infinity", "1e999", "0.1349999", "2.3260001"))
            assertNull(whiteBalanceTemperature(temperature, "6504", invalid))
    }

    @Test fun missingTemperatureSupportKeepsTheCoefficientEditorAvailable() {
        val coefficients = balance()
        assertNull(coefficients.temperature)
        assertNotNull(whiteBalanceGains(coefficients, listOf("2", "0.8", "1.5")))
        val supported = checkedWhiteBalance(rgb, 0.001f, 8f, 2, "user", settings, temperature())!!
        assertEquals(6504.125, supported.temperature!!.kelvin, 0.0)
    }

    @Test fun malformedNativeTemperatureBoundsDoNotProduceEditableValues() {
        assertNull(checkedWhiteBalanceTemperature(Double.NaN, 1.0, 1901.0, 25000.0, 0.135, 2.326))
        assertNull(checkedWhiteBalanceTemperature(6504.0, Double.POSITIVE_INFINITY, 1901.0, 25000.0, 0.135, 2.326))
        assertNull(checkedWhiteBalanceTemperature(6504.0, 1.0, 25000.0, 1901.0, 0.135, 2.326))
        assertNull(checkedWhiteBalanceTemperature(6504.0, 1.0, 1901.0, Double.NaN, 0.135, 2.326))
        assertNull(checkedWhiteBalanceTemperature(6504.0, 1.0, 1901.0, 25000.0, 0.0, 2.326))
        assertNull(checkedWhiteBalanceTemperature(6504.0, 1.0, 1901.0, 25000.0, 2.326, 0.135))
        assertNull(checkedWhiteBalanceTemperature(1900.0, 1.0, 1901.0, 25000.0, 0.135, 2.326))
        assertNull(checkedWhiteBalanceTemperature(6504.0, 3.0, 1901.0, 25000.0, 0.135, 2.326))
    }
}
