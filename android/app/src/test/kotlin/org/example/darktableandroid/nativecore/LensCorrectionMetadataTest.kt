/* SPDX-License-Identifier: GPL-3.0-or-later */
package org.example.darktableandroid.nativecore

import org.json.JSONObject
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Test

class LensCorrectionMetadataTest {
    private fun metadata() = JSONObject("""{
      "status":"ready","message":"Calibrated profile available","method":1,
      "embeddedAvailable":false,"databaseAvailable":true,"cameraMatched":true,"lensMatched":true,
      "supportedFlags":5,"camera":"Camera X","lens":"Lens 35mm",
      "cameras":[{"value":"Camera X","label":"Maker Camera X"}],
      "lenses":[{"value":"Lens 35mm","label":"Maker Lens 35mm"}]
    }""")

    @Test fun keepsCanonicalProfileIdentitySeparateFromItsDisplayLabel() {
        val info = requireNotNull(metadata().readLensCorrection())
        assertEquals(1, info.method)
        assertEquals(5, info.supportedFlags)
        assertEquals("Camera X", info.cameras.single().value)
        assertEquals("Maker Camera X", info.cameras.single().label)
        assertEquals("Lens 35mm", info.lenses.single().value)
    }

    @Test fun absentCatalogsAndDuplicateNativeValuesAreSafe() {
        val data = metadata()
        data.remove("cameras")
        data.getJSONArray("lenses").put(JSONObject("""{"value":"Lens 35mm","label":"Duplicate lens"}"""))
        val info = requireNotNull(data.readLensCorrection())
        assertEquals(emptyList<LensProfileChoice>(), info.cameras)
        assertEquals(1, info.lenses.size)
    }

    @Test fun malformedOptionalLensMetadataCannotHideTheRemainingModule() {
        val data = metadata().put("supportedFlags", 8)
        assertNull(data.readLensCorrection())
        val module = parseModules("""[{"operation":"lens","name":"Lens correction","enabled":true,
          "parameters":[{"name":"v_strength","type":"float","value":0,"min":0,"max":1}],
          "lensCorrection":$data}]""").single()
        assertEquals("v_strength", module.parameters.single().name)
        assertNull(module.lensCorrection)
    }

    @Test fun requiresActualNativeTypesAndBoundsRatherThanCoercingText() {
        for((key, value) in listOf("method" to 1.5, "method" to 3, "databaseAvailable" to "true",
            "supportedFlags" to "7", "status" to "", "camera" to "camera\u0000name")) {
            assertNull("$key=$value", metadata().put(key, value).readLensCorrection())
        }
        assertNotNull(metadata().put("message", JSONObject.NULL).readLensCorrection())
    }
}
