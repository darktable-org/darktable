/* SPDX-License-Identifier: GPL-3.0-or-later */
package org.example.darktableandroid

import org.example.darktableandroid.nativecore.LensCorrection
import org.example.darktableandroid.nativecore.ModuleParameter
import org.example.darktableandroid.nativecore.ModuleTextParameter
import org.example.darktableandroid.nativecore.ParameterChoice
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class LensCorrectionModelTest {
    private fun info(method: Int = 1, embedded: Boolean = true, database: Boolean = true,
                     matched: Boolean = true, flags: Int = 7) =
        LensCorrection("ready", null, method, embedded, database, true, matched, flags,
            "Camera", "Lens", emptyList(), emptyList())
    private fun field(name: String, value: Double = 1.0) =
        ModuleParameter(name, name, "float", value, 0.0, 1000.0, emptyList())
    private val all = listOf("modify_flags", "inverse", "target_geom", "scale", "tca_override", "tca_r", "tca_b",
        "focal", "aperture", "distance", "crop", "cor_dist_ft", "cor_vig_ft", "cor_ca_r_ft", "cor_ca_b_ft",
        "scale_md", "v_strength", "v_radius", "v_steepness", "lens.autodetect", "lens.autoscale").map { field(it) } +
        ModuleParameter("method", "correction method", "enum", 1.0, 0.0, 2.0,
            listOf(ParameterChoice(0.0, "embedded metadata"), ParameterChoice(1.0, "Lensfun"), ParameterChoice(2.0, "manual")))

    @Test fun manualMethodOnlyShowsItsOwnControlsAndRetainsUnknownGenericFields() {
        val controls = lensCorrectionControls(info(method = 2), all + field("blend.opacity"), emptyList())
        assertTrue(controls.shooting.isEmpty())
        assertTrue(controls.correction.isEmpty())
        assertEquals(listOf("lens.autodetect"), controls.actions.map { it.name })
        assertEquals(listOf("v_strength", "v_radius", "v_steepness"), controls.manualVignette.map { it.name })
        assertTrue("scale" in controls.names) // Inactive original fields cannot leak into the generic editor.
        assertFalse("blend.opacity" in controls.names)
    }

    @Test fun unavailableEmbeddedMethodHasNoInertChoicesOrFields() {
        val controls = lensCorrectionControls(info(method = 0, embedded = false), all, emptyList())
        assertEquals(listOf(1.0, 2.0), controls.method?.choices?.map { it.value })
        assertTrue(controls.correction.isEmpty())
        assertTrue(controls.shooting.isEmpty())
    }

    @Test fun embeddedCapabilitiesKeepOnlyTheirNativeFineTuningControls() {
        val controls = lensCorrectionControls(info(method = 0, flags = 4), all, emptyList())
        assertEquals(listOf("modify_flags", "scale_md", "cor_dist_ft"), controls.correction.map { it.name })
        assertTrue(controls.shooting.isEmpty())
        assertFalse(controls.correction.any { it.name in setOf("scale", "tca_r", "tca_b") })
    }

    @Test fun chromaticAberrationOnlyEmbeddedWarpKeepsItsSharedNativeStrength() {
        val native = listOf(field("modify_flags"), field("scale_md"),
            field("cor_dist_ft").copy(label = "embedded warp strength"))
        val controls = lensCorrectionControls(info(method = 0, flags = 1), native, emptyList())
        assertEquals(listOf("modify_flags", "scale_md", "cor_dist_ft"), controls.correction.map { it.name })
        assertEquals("embedded warp strength", controls.correction.last().label)
        assertFalse(controls.correction.any { it.name in setOf("cor_ca_r_ft", "cor_ca_b_ft") })
    }

    @Test fun missingProfileDoesNotOfferProfileTransformSliders() {
        val controls = lensCorrectionControls(info(matched = false), all, emptyList())
        assertTrue(controls.correction.isEmpty())
        assertEquals(listOf("focal", "aperture", "distance", "crop"), controls.shooting.map { it.name })
        assertFalse(controls.actions.any { it.name == "lens.autoscale" })
        val missingDatabase = lensCorrectionControls(info(database = false), all, emptyList())
        assertTrue(missingDatabase.shooting.isEmpty())
        assertTrue(missingDatabase.correction.isEmpty())
    }

    @Test fun unmatchedCameraCannotEnableLensSelectionOrClaimAnOlderLensAsItsCalibration() {
        val legacy = info().copy(cameraMatched = false, lensMatched = true, camera = "HERO12 Black",
            lens = "HERO4", imageCamera = "HERO12 Black")
        val text = listOf(ModuleTextParameter("camera", "camera model", legacy.camera, 127),
            ModuleTextParameter("lens", "lens model", legacy.lens, 127))
        val controls = lensCorrectionControls(legacy, all, text)
        assertTrue(controls.correction.isEmpty())
        assertTrue(controls.shooting.isEmpty())
        assertEquals(listOf("lens.autodetect"), controls.actions.map { it.name })
        assertNull(controls.lensProfile)
        assertEquals("lens.camera_profile", controls.cameraProfile?.name)
        assertTrue(controls.manualVignette.any { it.name == "v_strength" })
        assertTrue("scale" in controls.names && "lens" in controls.textNames)
    }

    @Test fun tcaOverrideHasToBeExplicitAndCanAddManualTcaWithoutACalibration() {
        val off = all.map { if(it.name == "tca_override") it.copy(value = 0.0) else it }
        val controls = lensCorrectionControls(info(flags = 4), off, emptyList())
        assertTrue(controls.correction.any { it.name == "tca_override" })
        assertFalse(controls.correction.any { it.name == "tca_r" })
        val on = lensCorrectionControls(info(flags = 4), all, emptyList())
        assertTrue(on.correction.any { it.name == "tca_r" })
        assertTrue(on.correction.any { it.name == "tca_b" })
    }

    @Test fun manualRadiusAndSteepnessAppearAfterStrengthIsUsed() {
        val off = lensCorrectionControls(info(), all.map { if(it.name == "v_strength") it.copy(value = 0.0) else it }, emptyList())
        assertEquals(listOf("v_strength"), off.manualVignette.map { it.name })
        val on = lensCorrectionControls(info(), all, emptyList())
        assertEquals(listOf("v_strength", "v_radius", "v_steepness"), on.manualVignette.map { it.name })
    }

    @Test fun profileSelectorsUseTheNativeSyntheticFieldsAndHideUnsafeFreeText() {
        val camera = ModuleTextParameter("lens.camera_profile", "camera profile", "Camera", 127)
        val lens = ModuleTextParameter("lens.profile", "lens profile", "Lens", 127)
        val text = listOf(camera, lens, camera.copy(name = "camera"), lens.copy(name = "lens"), camera.copy(name = "unknown"))
        val controls = lensCorrectionControls(info(), all, text)
        assertEquals(camera, controls.cameraProfile)
        assertEquals(lens, controls.lensProfile)
        assertEquals(setOf("lens.camera_profile", "lens.profile", "camera", "lens"), controls.textNames)
    }

    @Test fun originalTextBuffersEnableCanonicalProfileSelectorsWithoutInventingBounds() {
        val camera = ModuleTextParameter("camera", "camera model", "Uninitialized camera", 63)
        val lens = ModuleTextParameter("lens", "lens model", "Uninitialized lens", 95)
        val controls = lensCorrectionControls(info(), all, listOf(camera, lens))
        assertEquals(camera.copy(name = "lens.camera_profile", value = "Camera"), controls.cameraProfile)
        assertEquals(lens.copy(name = "lens.profile", value = "Lens"), controls.lensProfile)
        assertEquals(setOf("camera", "lens"), controls.textNames)
        val explicit = lens.copy(name = "lens.profile", maxBytes = 127)
        assertEquals(explicit, lensCorrectionControls(info(), all, listOf(camera, lens, explicit)).lensProfile)
    }
}
