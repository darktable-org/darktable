/* SPDX-License-Identifier: GPL-3.0-or-later */
package org.example.darktableandroid.nativecore

import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

class ToolFileMetadataTest {
    @Test fun nativeFileCapabilityRequiresAnExplicitBooleanAndAcceptedFormats() {
        val module = parseModules("""[{
          "operation":"lut3d","name":"3D LUT","enabled":true,
          "textParameters":[
            {"name":"file.import","label":"LUT file","value":"/private/copy.cube","maxBytes":1023,
              "file":true,"extensions":[".CUBE","3dl","png","bad/path"]},
            {"name":"bad-marker","value":"","maxBytes":1023,"file":"true","extensions":["png"]},
            {"name":"no-formats","value":"","maxBytes":1023,"file":true}
          ]
        }]""").single()
        val control = module.textParameters.single()
        assertTrue(control.isFile)
        assertEquals(listOf("cube", "3dl", "png"), control.extensions)
        assertTrue(control.accepts("/private/immutable-inputs/image-42/mask.png"))
    }

    @Test fun fileImportIsNeverPresentedAsAnEditableNativePath() {
        listOf("", ",\"file\":false", ",\"file\":null").forEach { capability ->
            val module = parseModules("""[{
              "operation":"rasterfile","name":"external raster masks","enabled":true,
              "textParameters":[{"name":"file.import","value":"/private/mask.png","maxBytes":2047$capability}]
            }]""").single()
            assertTrue(module.textParameters.isEmpty())
        }
    }
}
