/* SPDX-License-Identifier: GPL-3.0-or-later */
package org.example.darktableandroid.nativecore

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class ModuleInstanceTest {
    @Test fun olderOrUnsupportedRuntimeDoesNotExposeCreation() {
        val modules = parseModules("""[
            {"operation":"exposure","name":"exposure","enabled":true},
            {"operation":"temperature","name":"white balance","enabled":true,"canCreateInstance":false}
        ]""")
        assertTrue(modules.all { !it.canCreateInstance })
        assertNull(modules.instanceSource("exposure"))
        assertNull(modules.instanceSource("temperature"))
    }

    @Test fun resetRequiresAnExplicitNativeCapability() {
        val modules = parseModules("""[
            {"operation":"exposure","name":"exposure","enabled":true,"canReset":true},
            {"operation":"old","name":"old runtime","enabled":true},
            {"operation":"malformed","name":"malformed","enabled":true,"canReset":"true"}
        ]""")
        assertTrue(modules[0].canReset)
        assertFalse(modules[1].canReset)
        assertFalse(modules[2].canReset)
    }

    @Test fun onlyExplicitBooleanCapabilityEnablesCreation() {
        val modules = parseModules("""[
            {"operation":"exposure","name":"exposure","enabled":true,"canCreateInstance":true},
            {"operation":"contrast","name":"contrast","enabled":false,"canCreateInstance":"true"},
            {"operation":"temperature","name":"white balance","enabled":true,"canCreateInstance":1}
        ]""")
        assertTrue(modules[0].canCreateInstance)
        assertFalse(modules[1].canCreateInstance)
        assertFalse(modules[2].canCreateInstance)
        assertEquals("exposure", modules.instanceSource("exposure")?.operation)
        assertNull(modules.instanceSource("contrast"))
    }

    @Test fun sourceLookupUsesTheExactLiveInstanceIdentifier() {
        val modules = parseModules("""[
            {"operation":"exposure","name":"exposure","enabled":true,"canCreateInstance":true},
            {"operation":"exposure@7","name":"exposure 7","enabled":false,"canCreateInstance":true}
        ]""")
        assertEquals("exposure 7", modules.instanceSource("exposure@7")?.name)
        assertEquals("exposure", modules.instanceSource("exposure")?.name)
        assertNull(modules.instanceSource("exposure@1"))
        assertNull(modules.instanceSource("exposure@07"))
        assertNull(modules.instanceSource("exposure@7@0"))
    }

    @Test fun refreshKeepsLiveSelectionAndUndoClearsRetiredSelection() {
        val refreshed = parseModules("""[
            {"operation":"exposure@7","name":"exposure 7","enabled":true,"canCreateInstance":true},
            {"operation":"exposure","name":"exposure","enabled":true,"canCreateInstance":true}
        ]""")
        assertEquals("exposure@7", retainModuleSelection(refreshed, "exposure@7"))
        // Undoing creation must not silently redirect a retired ID to its base.
        val afterUndo = refreshed.filter { it.operation != "exposure@7" }
        assertNull(retainModuleSelection(afterUndo, "exposure@7"))
        assertNull(afterUndo.instanceSource("exposure@7"))
        assertEquals("exposure", retainModuleSelection(afterUndo, "exposure"))
        assertNull(retainModuleSelection(refreshed, null))
    }

    @Test fun ambiguousMetadataCannotSelectOrCreateAnInstance() {
        val modules = parseModules("""[
            {"operation":"exposure@7","name":"first","enabled":true,"canCreateInstance":true},
            {"operation":"exposure@7","name":"second","enabled":true,"canCreateInstance":true}
        ]""")
        assertNull(modules.instanceSource("exposure@7"))
        assertNull(retainModuleSelection(modules, "exposure@7"))
    }
}
