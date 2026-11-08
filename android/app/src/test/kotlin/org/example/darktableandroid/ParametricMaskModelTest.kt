/* SPDX-License-Identifier: GPL-3.0-or-later */
package org.example.darktableandroid

import org.example.darktableandroid.nativecore.ModuleParameter
import org.example.darktableandroid.nativecore.parseNumericInput
import org.example.darktableandroid.nativecore.resetValue
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class ParametricMaskModelTest {
    private fun field(channel: Int, name: String, value: Double, label: String = "input gray · $name") =
        ModuleParameter("blend.parametric.$channel.$name", label, if(name.startsWith("p")) "float" else "bool",
            value.toFloat().toDouble(), 0.0, 1.0, emptyList())

    private fun channel(index: Int = 0) = listOf(
        field(index, "p0", 0.1), field(index, "p1", 0.3), field(index, "p2", 0.7), field(index, "p3", 0.9),
        field(index, "enabled", 1.0), field(index, "inverted", 0.0),
    )

    @Test fun nativeChannelNumbersDetermineInputAndOutputWithoutRelabelingAddresses() {
        val controls = parametricMaskControls(channel(14) + channel(1) + channel(8))
        assertEquals(listOf(1, 8, 14), controls.channels.map { it.index })
        assertEquals(listOf(false, false, true), controls.channels.map { it.output })
        assertEquals("blend.parametric.14.p2", controls.channels.last().thresholds[2].name)
        assertEquals("input gray", controls.channels.first().label)
    }

    @Test fun everyHandleUsesItsActualNeighborBoundsAndRejectsCrossingInput() {
        val group = parametricMaskControls(channel()).channels.single()
        val first = requireNotNull(group.boundedThreshold(0))
        val second = requireNotNull(group.boundedThreshold(1))
        val last = requireNotNull(group.boundedThreshold(3))
        assertEquals(0.0, first.minimum!!, 0.0)
        assertEquals(group.thresholds[1].value, first.maximum!!, 0.0)
        assertEquals(group.thresholds[0].value, second.minimum!!, 0.0)
        assertEquals(group.thresholds[2].value, second.maximum!!, 0.0)
        assertEquals(group.thresholds[2].value, last.minimum!!, 0.0)
        assertEquals(1.0, last.maximum!!, 0.0)
        assertNull(second.parseNumericInput("0.8"))
        assertEquals(group.thresholds[0].value, second.parseNumericInput("0.1")!!, 0.0)
        assertNull(group.boundedThreshold(4))
    }

    @Test fun handleResetDoesNotOfferADefaultThatWouldCrossAnotherHandle() {
        val parameters = channel().map { if(it.name.endsWith(".p2")) it.copy(defaultValue = 1.0) else it }
        val group = parametricMaskControls(parameters).channels.single()
        assertNull(requireNotNull(group.boundedThreshold(2)).resetValue())
    }

    @Test fun importedOrPartialThresholdSetsStayWithTheGenericRenderer() {
        assertTrue(parametricMaskControls(channel().drop(1)).channels.isEmpty())
        assertTrue(parametricMaskControls(channel() + channel().first()).channels.isEmpty())
        assertTrue(parametricMaskControls(channel().map {
            if(it.name.endsWith(".p0")) it.copy(value = 0.8) else it
        }).channels.isEmpty())
        assertTrue(parametricMaskControls(channel().map {
            if(it.name.endsWith(".p1")) it.copy(value = Double.NaN) else it
        }).channels.isEmpty())
        assertTrue(parametricMaskControls(channel(11)).channels.isEmpty())
    }

    @Test fun unknownFieldsAreNotClaimedByTheDedicatedEditor() {
        val unknown = field(0, "p4", 0.5)
        val controls = parametricMaskControls(channel() + unknown)
        assertEquals(6, controls.names.size)
        assertFalse(unknown.name in controls.names)
        val enable = ModuleParameter("blend.parametric.enabled", "enable parametric mask", "bool",
            0.0, 0.0, 1.0, emptyList())
        assertEquals(listOf(enable), parametricMaskControls(listOf(enable)).global)
    }

    @Test fun selectionDiagramShowsFeatheringInversionAndDisabledChannels() {
        val group = parametricMaskControls(channel()).channels.single()
        assertEquals(0.0, parametricThresholdDiagramValue(group, 0.0)!!, 0.0)
        assertEquals(0.5, parametricThresholdDiagramValue(group, 0.2)!!, 0.000001)
        assertEquals(1.0, parametricThresholdDiagramValue(group, 0.5)!!, 0.0)
        assertEquals(0.5, parametricThresholdDiagramValue(group, 0.8)!!, 0.000001)
        assertEquals(0.0, parametricThresholdDiagramValue(group, 1.0)!!, 0.0)
        val inverted = group.copy(controls = group.controls.map {
            if(it.name.endsWith(".inverted")) it.copy(value = 1.0) else it
        })
        assertEquals(1.0, parametricThresholdDiagramValue(inverted, 0.0)!!, 0.0)
        assertEquals(0.0, parametricThresholdDiagramValue(inverted, 0.5)!!, 0.0)
        val disabled = inverted.copy(controls = inverted.controls.map {
            if(it.name.endsWith(".enabled")) it.copy(value = 0.0) else it
        })
        assertEquals(1.0, parametricThresholdDiagramValue(disabled, 0.5)!!, 0.0)
        assertNull(parametricThresholdDiagramValue(group, Double.NaN))
    }
}
