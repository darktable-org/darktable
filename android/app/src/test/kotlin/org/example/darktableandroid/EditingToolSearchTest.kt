/* SPDX-License-Identifier: GPL-3.0-or-later */
package org.example.darktableandroid

import org.example.darktableandroid.nativecore.EditorModule
import org.example.darktableandroid.nativecore.ModuleParameter
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

class EditingToolSearchTest {
    private val exposure = EditorModule("exposure", "Exposure", true, true,
        listOf(ModuleParameter("black", "Black point", "float", 0.0, -0.1, 0.1, emptyList())))
    private val mixer = EditorModule("channelmixerrgb@3", "Color calibration 3", false, true,
        listOf(ModuleParameter("red[1]", "Red output / Green input", "float", 0.0, -2.0, 2.0, emptyList())))

    @Test fun searchUsesNativeControlLabelsAndAllSearchWords() {
        assertEquals(listOf(mixer), filterEditingTools(listOf(exposure, mixer), " GREEN   output ", false))
        assertEquals(listOf(exposure), filterEditingTools(listOf(exposure, mixer), "black", false))
        assertTrue(filterEditingTools(listOf(exposure, mixer), "black calibration", false).isEmpty())
    }

    @Test fun enabledFilterAndSearchComposeWithoutChangingInstanceIdsOrOrder() {
        val modules = listOf(mixer, exposure)
        assertEquals(modules, filterEditingTools(modules, "  ", false))
        assertEquals(listOf(exposure), filterEditingTools(modules, "", true))
        assertEquals(listOf(mixer), filterEditingTools(modules, "channelmixerrgb@3", false))
        assertTrue(filterEditingTools(modules, "channelmixerrgb", true).isEmpty())
    }

    @Test fun toolNameTermsDoNotHideTheMatchingControlsInsideTheTool() {
        val words = controlSearchWords(mixer, "color calibration GREEN")
        assertEquals(listOf("GREEN"), words)
        assertTrue(matchesControlSearch(words, "red[1]", "Red output / Green input"))
        assertEquals(false, matchesControlSearch(words, "red[0]", "Red output / Red input"))
        assertTrue(controlSearchWords(exposure, "exposure").isEmpty())
    }
}
