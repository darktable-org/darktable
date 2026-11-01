/* SPDX-License-Identifier: GPL-3.0-or-later */
package org.example.darktableandroid

import org.example.darktableandroid.nativecore.EditorModule
import org.example.darktableandroid.nativecore.ModuleParameter
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertSame
import org.junit.Assert.assertTrue
import org.junit.Test

class EditorToolCategoriesTest {
    private fun tool(operation: String, enabled: Boolean = false, label: String = operation) =
        EditorModule(operation, label, enabled, true,
            listOf(ModuleParameter("amount", "Adjustment amount", "float", 0.5, 0.0, 1.0, emptyList())))

    @Test fun classificationUsesTheNativeFamilyWithoutReplacingInstanceIdentity() {
        val modules = listOf(tool("exposure@7"), tool("retouch@2"), tool("crop@4"), tool("temperature"))
        assertEquals(listOf("exposure@7", "temperature"),
            modules.workspaceModules(WorkspaceTab.ADJUSTMENTS).map { it.operation })
        assertEquals(listOf("retouch@2"), modules.workspaceModules(WorkspaceTab.DETAIL).map { it.operation })
        assertEquals(listOf("crop@4"), modules.workspaceModules(WorkspaceTab.GEOMETRY).map { it.operation })
        assertSame(modules[0], modules.preferredWorkspaceModule(WorkspaceTab.ADJUSTMENTS, "exposure@7"))
        assertTrue(modules.workspaceModules(WorkspaceTab.SETTINGS).isEmpty())
        assertNull(modules.preferredWorkspaceModule(WorkspaceTab.SETTINGS, "crop@4"))
    }

    @Test fun futureToolsRemainVisibleAndNoCategoryFilterMutatesTheModuleList() {
        val future = tool("future-native-tool@11", true)
        val geometry = tool("rotatepixels", true)
        val modules = listOf(geometry, future, tool("demosaic", true), tool("rawprepare", true))
        val before = modules.toList()
        val grouped = WorkspaceTab.entries.flatMap { modules.workspaceModules(it) }
        assertEquals(modules.toSet(), grouped.toSet())
        assertEquals(modules.size, grouped.size)
        assertEquals(listOf(future, modules[3]), modules.workspaceModules(WorkspaceTab.ADJUSTMENTS))
        assertEquals(before, modules)
        assertTrue(future.enabled)
    }

    @Test fun categorySearchAndEnabledFilterPreserveNativeOrderAndScope() {
        val offInstance = tool("exposure@7", false, "Exposure 7")
        val enabledColor = tool("colorbalance", true, "Color balance")
        val crop = tool("crop", true, "Crop")
        val modules = listOf(offInstance, crop, enabledColor)
        assertEquals(listOf(offInstance, enabledColor), modules.workspaceModules(WorkspaceTab.ADJUSTMENTS))
        assertEquals(listOf(enabledColor), modules.workspaceModules(WorkspaceTab.ADJUSTMENTS, "amount", true))
        assertEquals(listOf(offInstance), modules.workspaceModules(WorkspaceTab.ADJUSTMENTS, "exposure@7"))
        assertTrue(modules.workspaceModules(WorkspaceTab.DETAIL, "crop").isEmpty())
        assertFalse(offInstance.enabled)
    }

    @Test fun liveRememberedInstanceWinsWithoutEnablingItAndRetiredInstancesAreDiscarded() {
        val base = tool("exposure", true)
        val instance = tool("exposure@9", false)
        val crop = tool("crop", true)
        val live = listOf(base, instance, crop)
        assertSame(instance, live.preferredWorkspaceModule(WorkspaceTab.ADJUSTMENTS, instance.operation))
        assertFalse(instance.enabled)
        val afterUndo = listOf(base, crop)
        assertSame(base, afterUndo.preferredWorkspaceModule(WorkspaceTab.ADJUSTMENTS, instance.operation))
        assertSame(crop, afterUndo.preferredWorkspaceModule(WorkspaceTab.GEOMETRY, instance.operation))
        assertNull(afterUndo.preferredWorkspaceModule(WorkspaceTab.DETAIL, instance.operation))
    }

    @Test fun categoryDefaultsPreferEnabledEditableToolsThenUsefulDisabledTools() {
        val sharpen = tool("sharpen", true)
        val denoise = tool("denoiseprofile", false)
        val repair = tool("retouch", true)
        val modules = listOf(repair, denoise, sharpen)
        assertSame(sharpen, modules.preferredWorkspaceModule(WorkspaceTab.DETAIL))
        assertSame(repair, listOf(denoise, repair).preferredWorkspaceModule(WorkspaceTab.DETAIL))
        assertSame(denoise, listOf(denoise, tool("diffuse")).preferredWorkspaceModule(WorkspaceTab.DETAIL))
        assertFalse(denoise.enabled)
    }

    @Test fun filteredOutSelectionAndNewSessionsCannotSelectAnUnrelatedOrMissingIdentity() {
        val old = tool("crop@12", false)
        val active = tool("crop", true)
        val shown = listOf(old, active).workspaceModules(WorkspaceTab.GEOMETRY, enabledOnly = true)
        assertSame(active, shown.preferredWorkspaceModule(WorkspaceTab.GEOMETRY, old.operation))
        val nextSession = listOf(tool("exposure"), tool("sharpen"))
        assertNull(nextSession.preferredWorkspaceModule(WorkspaceTab.GEOMETRY, old.operation))
        assertEquals("exposure", nextSession.preferredWorkspaceModule(WorkspaceTab.ADJUSTMENTS, old.operation)?.operation)
    }
}
