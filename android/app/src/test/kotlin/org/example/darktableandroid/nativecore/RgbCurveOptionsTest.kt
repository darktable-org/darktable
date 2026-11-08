/* SPDX-License-Identifier: GPL-3.0-or-later */
package org.example.darktableandroid.nativecore

import org.junit.Assert.*
import org.junit.Test

class RgbCurveOptionsTest {
    private val modes = listOf(CurveOptionChoice(0, "linked"), CurveOptionChoice(1, "independent"))
    private val colors = (0..6).map { CurveOptionChoice(it, "color $it") }

    @Test fun independentModeRetainsTheChosenLinkedColorMethod() {
        val options = checkedRgbCurveOptions(0, 4, modes, colors)!!
        assertTrue(options.accepts(1, 4))
        assertTrue(options.accepts(0, 4))
        assertFalse(options.accepts(2, 4))
        assertFalse(options.accepts(1, -1))
        assertFalse(options.accepts(1, 7))
        assertEquals(4, options.preserveColors)
    }

    @Test fun malformedOrUnavailableNativeChoicesDoNotBecomeEditable() {
        assertNull(checkedRgbCurveOptions(2, 1, modes, colors))
        assertNull(checkedRgbCurveOptions(0, 7, modes, colors))
        assertNull(checkedRgbCurveOptions(0, 1, emptyList(), colors))
        assertNull(checkedRgbCurveOptions(0, 1, modes + modes[0], colors))
        assertNull(checkedRgbCurveOptions(0, 1, modes, colors + CurveOptionChoice(7, "unknown")))
        assertNull(checkedRgbCurveOptions(0, 1, modes, listOf(CurveOptionChoice(1, " "))))
        val restricted = checkedRgbCurveOptions(0, 1, modes.take(1), colors.take(2))!!
        assertFalse(restricted.accepts(1, 1))
        assertFalse(restricted.accepts(0, 2))
    }

    @Test fun laterCallerListChangesCannotAlterTheCapturedCapabilities() {
        val mutableModes = modes.toMutableList()
        val mutableColors = colors.toMutableList()
        val options = checkedRgbCurveOptions(0, 1, mutableModes, mutableColors)!!
        mutableModes.clear()
        mutableColors.clear()
        assertTrue(options.accepts(1, 6))
        assertEquals(modes, options.modes)
        assertEquals(colors, options.preserveColorChoices)
    }
}
