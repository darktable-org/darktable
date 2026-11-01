/* SPDX-License-Identifier: GPL-3.0-or-later */
package org.example.darktableandroid.nativecore

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class TextParameterInputTest {
    private val control = ModuleTextParameter("text", "Watermark text", "", 10)

    private fun lensModule() = EditorModule("lens@7", "Lens correction", true, true, emptyList(),
        textParameters = listOf(ModuleTextParameter("camera", "Camera", "", 12),
            ModuleTextParameter("lens", "Lens", "", 10)),
        lensCorrection = LensCorrection("missing_lens", null, 1, false, true, false, false, 0, "", "",
            listOf(LensProfileChoice("Camera Model", "Camera Model")),
            listOf(LensProfileChoice("Lens Model", "Lens Model"), LensProfileChoice("Too Long Lens Model", "Too Long Lens Model"))))

    @Test fun lensProfileWritesKeepCanonicalActionNamesAndNativeByteLimits() {
        val module = lensModule()
        val camera = module.textParameterForWrite("lens.camera_profile", "Camera Model")
        assertEquals("lens.camera_profile", camera?.name)
        assertEquals(12, camera?.maxBytes)
        assertEquals("lens.profile", module.textParameterForWrite("lens.profile", "Lens Model")?.name)
        assertNull(module.textParameterForWrite("lens.profile", "Too Long Lens Model"))
        assertNull(module.textParameterForWrite("lens.profile", "Unlisted"))
    }

    @Test fun profileActionsRequireLensCapabilityAndTheActiveNativeCatalog() {
        val module = lensModule()
        assertNull(module.copy(operation = "watermark").textParameterForWrite("lens.profile", "Lens Model"))
        assertNull(module.copy(lensCorrection = null).textParameterForWrite("lens.profile", "Lens Model"))
        assertNull(module.copy(lensCorrection = module.lensCorrection!!.copy(method = 2))
            .textParameterForWrite("lens.profile", "Lens Model"))
        assertEquals("camera", module.textParameterForWrite("camera", "Manual")?.name)
    }

    @Test fun utf8PayloadLimitCountsMultibyteCharactersAndEmoji() {
        assertTrue(control.accepts(""))
        assertTrue(control.accepts("相片📷")) // 3 + 3 + 4 bytes, exactly the native limit.
        assertFalse(control.accepts("相片📷!"))
        assertTrue(control.accepts("0123456789"))
        assertFalse(control.accepts("01234567890"))
    }

    @Test fun embeddedNulAndMalformedSurrogatesNeverReachNativeFixedBuffers() {
        assertFalse(control.accepts("a\u0000b"))
        assertFalse(control.accepts("\uD800"))
        assertFalse(control.accepts("\uDC00"))
        assertFalse(control.accepts("\uD800a"))
        assertTrue(control.accepts("\uD83D\uDCF7"))
    }

    @Test fun templateFileNamesUseTheNativeResourceBasenameRule() {
        val template = control.copy(name = "filename", maxBytes = 64)
        assertTrue(template.accepts("darktable.svg"))
        listOf("", ".", "..", "/tmp/template.svg", "folder/template.svg", "folder\\template.svg").forEach {
            assertFalse(template.accepts(it))
        }
    }

    @Test fun textMetadataEnablesTextOnlyModulesAndRejectsMalformedLimitsOrDefaults() {
        val module = parseModules("""[{
            "operation":"watermark", "name":"watermark", "enabled":true,
            "textParameters":[
              {"name":"text","label":"Watermark text","value":"Current","maxBytes":10,"default":"Original"},
              {"name":"font","value":"Sans","maxBytes":4,"default":"Too long"},
              {"name":"bad","value":"","maxBytes":4.5}
            ]
        }]""").single()
        assertTrue(module.hasEditableControls)
        assertEquals(2, module.textParameters.size)
        assertEquals("Original", module.textParameters[0].defaultValue)
        assertNull(module.textParameters[1].defaultValue)
        assertEquals(emptyList<ModuleTextParameter>(), parseModules("""[
          {"operation":"old","name":"older runtime","enabled":true}
        ]""").single().textParameters)
    }
}
