/* SPDX-License-Identifier: GPL-3.0-or-later */
package org.example.darktableandroid.nativecore

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test

class ParameterInputTest {
    private fun parameter(type: String, value: Double = 0.0, minimum: Double? = null, maximum: Double? = null) =
        ModuleParameter("value", "Value", type, value, minimum, maximum, emptyList())

    @Test fun decimalFloatBoundsUseTheValueNativeCodeWillStore() {
        val minimum = 0.1f.toDouble()
        val maximum = 0.2f.toDouble()
        val control = parameter("float", value = minimum, minimum = minimum, maximum = maximum)

        // Decimal 0.1 is smaller as a Double than the native Float minimum.
        // Normalizing it also makes the unchanged input compare equal.
        assertEquals(control.value, control.parseNumericInput("0.1")!!, 0.0)
        assertEquals(maximum, control.parseNumericInput("0.2")!!, 0.0)
        assertNull(control.parseNumericInput("0.099"))
        assertNull(control.parseNumericInput("0.201"))
    }

    @Test fun nonFiniteInputAndFiniteDoubleOverflowingFloatAreRejected() {
        val control = parameter("float")
        for(input in listOf("NaN", "Infinity", "-Infinity", "1e309", "-1e309", "1e39", "-1e39", "", "invalid")) {
            assertNull(input, control.parseNumericInput(input))
        }
    }

    @Test fun doubleInputKeepsPrecisionAndRangeBeyondFloat() {
        val control = parameter("double")
        assertEquals(0.12345678901234567, control.parseNumericInput("0.12345678901234567")!!, 0.0)
        assertEquals(1e100, control.parseNumericInput("1e100")!!, 0.0)
        assertNull(control.parseNumericInput("Infinity"))
        assertNull(parameter("double", minimum = 0.1).parseNumericInput("0.0999999999"))
    }

    @Test fun integerInputRejectsFractionsAndHonorsBounds() {
        val control = parameter("int", minimum = -10.0, maximum = 100.0)
        assertNull(control.parseNumericInput("1.5"))
        assertNull(control.parseNumericInput("-0.5"))
        assertNull(control.parseNumericInput("101"))
        assertNull(control.parseNumericInput("-11"))
        assertEquals(-10.0, control.parseNumericInput("-10.0")!!, 0.0)
        assertEquals(100.0, control.parseNumericInput("1e2")!!, 0.0)
    }
}
