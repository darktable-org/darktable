/* SPDX-License-Identifier: GPL-3.0-or-later */
package org.example.darktableandroid.nativecore

import org.json.JSONArray
import org.json.JSONObject
import org.junit.Assert.*
import org.junit.Test

class WhiteBalancePresetMetadataTest {
    private fun metadata() = JSONObject("""{
      "channels":[{"index":2,"label":"cyan","value":0.7},{"index":3,"label":"yellow","value":1.3},
        {"index":1,"label":"magenta","value":1.1},{"index":0,"label":"green","value":1.8}],
      "manualMin":0.001,"manualMax":8,"setting":8,"settingLabel":"cloudy",
      "settings":[{"id":0,"label":"as shot"}],
      "temperature":{"kelvin":6504,"tint":1,"minKelvin":1901,"maxKelvin":25000,"minTint":0.135,"maxTint":2.326},
      "cameraPresets":{"camera":"Maker Four Channel","presets":[
        {"id":6,"label":"daylight","tuning":[0],"defaultTuning":0},
        {"id":8,"label":"cloudy","tuning":[3,-4,0],"defaultTuning":0}],
        "selectedPreset":8,"selectedTuning":-4}
    }""")

    private fun read(data: JSONObject = metadata()): WhiteBalance = requireNotNull(parseModules(JSONArray().put(
        JSONObject().put("operation", "temperature").put("name", "white balance").put("enabled", true)
            .put("whiteBalance", data)).toString()).single().whiteBalance)

    @Test fun retainsNativeIdentityAndOnlyTheActualFourChannelTuningChoices() {
        val balance = read()
        val catalog = requireNotNull(balance.cameraPresets)
        assertEquals("Maker Four Channel", catalog.camera)
        assertEquals(listOf(6, 8), catalog.presets.map { it.id })
        assertEquals(listOf(-4, 0, 3), catalog.presets[1].tuning)
        assertEquals(8, catalog.selectedPreset)
        assertEquals(-4, catalog.selectedTuning)
        assertTrue(balance.acceptsPreset(8, -4))
        assertTrue(balance.acceptsPreset(8, 0))
        assertTrue(balance.acceptsPreset(8, 3))
        for(hole in listOf(-5, -3, -2, -1, 1, 2, 4)) assertFalse(balance.acceptsPreset(8, hole))
        assertFalse(balance.acceptsPreset(7, 0))
        assertFalse(balance.acceptsPreset(0, 0))
        assertArrayEquals(floatArrayOf(1.8f, 1.1f, 0.7f, 1.3f),
            whiteBalanceGains(balance, listOf("0.7", "1.3", "1.1", "1.8")), 0f)
    }

    @Test fun anInactiveCatalogKeepsChoicesWithoutPretendingAPresetIsApplied() {
        val data = metadata().put("setting", 2).put("settingLabel", "user")
        data.getJSONObject("cameraPresets").put("selectedPreset", JSONObject.NULL).put("selectedTuning", JSONObject.NULL)
        val balance = read(data)
        val catalog = requireNotNull(balance.cameraPresets)
        assertNull(catalog.selectedPreset)
        assertNull(catalog.selectedTuning)
        assertEquals(0, catalog.presets[1].defaultTuning)
        assertTrue(balance.acceptsPreset(8, 3))
        assertEquals("user", balance.settingLabel)
    }

    @Test fun absentEmptyOrMalformedCatalogCannotHideTheBaseWhiteBalanceEditor() {
        val absent = metadata().also { it.remove("cameraPresets") }
        val variants = listOf(absent, metadata().put("cameraPresets", JSONObject.NULL),
            metadata().put("cameraPresets", "unavailable"), metadata().also {
                it.getJSONObject("cameraPresets").put("presets", JSONArray())
            }, metadata().also { it.getJSONObject("cameraPresets").put("camera", " ") })
        for(data in variants) {
            val balance = read(data)
            assertNull(balance.cameraPresets)
            assertEquals(4, balance.channels.size)
            assertNotNull(balance.temperature)
            assertTrue(balance.acceptsSetting(0))
            assertNotNull(whiteBalanceGains(balance, listOf("1", "1", "1", "1")))
        }
    }

    @Test fun rejectsMalformedPresetsAndTuningRatherThanCoercingOrFillingHoles() {
        val invalidPresets = listOf(
            """{"id":5,"label":"invalid id","tuning":[0],"defaultTuning":0}""",
            """{"id":"6","label":"text id","tuning":[0],"defaultTuning":0}""",
            """{"id":6.5,"label":"fractional id","tuning":[0],"defaultTuning":0}""",
            """{"id":6,"label":"","tuning":[0],"defaultTuning":0}""",
            """{"id":6,"label":"empty","tuning":[],"defaultTuning":0}""",
            """{"id":6,"label":"duplicate","tuning":[0,0],"defaultTuning":0}""",
            """{"id":6,"label":"gap default","tuning":[-4,3],"defaultTuning":0}""",
            """{"id":6,"label":"fractional tuning","tuning":[0,1.5],"defaultTuning":0}""",
            """{"id":6,"label":"text tuning","tuning":[0,"1"],"defaultTuning":0}""",
            """{"id":6,"label":"boolean tuning","tuning":[0,true],"defaultTuning":0}""",
            """{"id":6,"label":"overflow tuning","tuning":[0,2147483648],"defaultTuning":0}""",
        )
        for(preset in invalidPresets) {
            val data = metadata()
            data.getJSONObject("cameraPresets").put("presets", JSONArray().put(JSONObject(preset)))
                .put("selectedPreset", JSONObject.NULL).put("selectedTuning", JSONObject.NULL)
            assertNull(preset, read(data).cameraPresets)
        }
        val duplicate = metadata()
        duplicate.getJSONObject("cameraPresets").getJSONArray("presets")
            .put(JSONObject("""{"id":8,"label":"duplicate id","tuning":[0],"defaultTuning":0}"""))
        assertNull(read(duplicate).cameraPresets)
    }

    @Test fun rejectsPartialOrUnadvertisedSelectionInsteadOfDisplayingAnotherTuning() {
        for((preset, tuning) in listOf(8 to JSONObject.NULL, JSONObject.NULL to 0, 7 to 0, 8 to 1,
            "8" to -4, 8 to "-4", 8 to -4.5)) {
            val data = metadata()
            data.getJSONObject("cameraPresets").put("selectedPreset", preset).put("selectedTuning", tuning)
            assertNull("$preset / $tuning", read(data).cameraPresets)
            assertNotNull(read(data).temperature)
        }
    }

    @Test fun nativeSelectionRefreshRetainsBothPresetAndFineTuning() {
        val applied = read()
        val undo = metadata().put("setting", 6).put("settingLabel", "daylight")
        undo.getJSONObject("cameraPresets").put("selectedPreset", 6).put("selectedTuning", 0)
        val restored = read(undo)
        assertEquals(8, applied.cameraPresets!!.selectedPreset)
        assertEquals(-4, applied.cameraPresets!!.selectedTuning)
        assertEquals(6, restored.cameraPresets!!.selectedPreset)
        assertEquals(0, restored.cameraPresets!!.selectedTuning)
    }
}
