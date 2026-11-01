/* SPDX-License-Identifier: GPL-3.0-or-later */
package org.example.darktableandroid

import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.MaterialTheme
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.test.assertIsDisplayed
import androidx.compose.ui.test.assertIsNotEnabled
import androidx.compose.ui.test.junit4.createComposeRule
import androidx.compose.ui.test.onNodeWithContentDescription
import androidx.compose.ui.test.onNodeWithText
import androidx.compose.ui.test.performClick
import androidx.compose.ui.test.performScrollTo
import androidx.compose.ui.test.performTextReplacement
import androidx.compose.ui.test.performTouchInput
import androidx.test.platform.app.InstrumentationRegistry
import org.example.darktableandroid.nativecore.LensCorrection
import org.example.darktableandroid.nativecore.LensProfileChoice
import org.example.darktableandroid.nativecore.ModuleParameter
import org.example.darktableandroid.nativecore.ModuleTextParameter
import org.example.darktableandroid.nativecore.ParameterChoice
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Rule
import org.junit.Test

/** Production callbacks and mode presentation; original-engine pixels are checked separately. */
class LensCorrectionEditorTest {
    @get:Rule val compose = createComposeRule()
    private val context get() = InstrumentationRegistry.getInstrumentation().targetContext
    private val profiles = listOf(LensProfileChoice("Native Lens 35mm", "Current maker 35mm"),
        LensProfileChoice("Native Lens 85mm", "Other maker portrait lens"))
    private fun info(method: Int = 1, matched: Boolean = true) = LensCorrection(
        if(matched) "ready" else "lens_not_found", if(matched) "Profile found" else "No calibrated lens profile matches this image.",
        method, false, true, true, matched, if(matched) 7 else 0, "Native Camera", "Native Lens 35mm",
        listOf(LensProfileChoice("Native Camera", "Camera maker")), profiles)
    private fun controls(info: LensCorrection): LensCorrectionControls {
        val parameters = listOf(
            ModuleParameter("method", "Correction method", "enum", info.method.toDouble(), 0.0, 2.0,
                listOf(ParameterChoice(0.0, "Embedded metadata"), ParameterChoice(1.0, "Lensfun database"),
                    ParameterChoice(2.0, "Only manual vignette"))),
            ModuleParameter("v_strength", "Vignette strength", "float", 0.0, 0.0, 1.0, emptyList()),
            ModuleParameter("scale", "Lens image scale", "float", 1.0, 0.1, 2.0, emptyList()),
            ModuleParameter("cor_dist_ft", "Metadata distortion", "float", 1.0, 0.0, 2.0, emptyList()),
            ModuleParameter("lens.autodetect", "Detect from image", "action", 1.0, 1.0, 1.0, emptyList()),
        )
        return lensCorrectionControls(info, parameters, listOf(
            ModuleTextParameter("camera", "Camera model", info.camera, 127),
            ModuleTextParameter("lens", "Lens model", info.lens, 127)))
    }

    @Test fun manualModeShowsOnlyLiveFieldsAndOneNativeEditPerCompletedDrag() {
        val info = info(method = 2)
        val edits = mutableListOf<Pair<String, Double>>()
        compose.setContent { MaterialTheme {
            Column(Modifier.verticalScroll(rememberScrollState())) {
                LensCorrectionEditor(info, controls(info), true, { name, value -> edits += name to value }, { _, _ -> })
            }
        } }
        compose.onNodeWithText("Lens image scale").assertDoesNotExist()
        compose.onNodeWithText("Metadata distortion").assertDoesNotExist()
        val slider = compose.onNodeWithContentDescription(context.getString(R.string.parameter_slider_description, "Vignette strength"))
        slider.performScrollTo().performTouchInput {
            down(Offset(width / 4f, centerY))
            moveTo(Offset(width * .8f, centerY))
        }
        compose.runOnIdle { assertTrue(edits.isEmpty()) }
        slider.performTouchInput { up() }
        compose.runOnIdle {
            assertEquals(1, edits.size)
            assertEquals("v_strength", edits.single().first)
            assertTrue(edits.single().second > 0.0)
        }
    }

    @Test fun searchedProfileCommitsCanonicalNativeValueAndNotItsDisplayLabel() {
        val info = info()
        val edits = mutableListOf<Pair<String, String>>()
        compose.setContent { MaterialTheme {
            Column(Modifier.verticalScroll(rememberScrollState())) {
                LensCorrectionEditor(info, controls(info), true, { _, _ -> }, { name, value -> edits += name to value })
            }
        } }
        compose.onNodeWithContentDescription(context.getString(R.string.lens_profile)).performScrollTo().performClick()
        compose.onNodeWithText(context.getString(R.string.lens_profile_search)).performTextReplacement("other portrait")
        compose.runOnIdle { assertTrue("Searching the catalog is not a native edit", edits.isEmpty()) }
        compose.onNodeWithText("Other maker portrait lens").performClick()
        compose.runOnIdle { assertEquals(listOf("lens.profile" to "Native Lens 85mm"), edits) }
    }

    @Test fun missingProfileExplainsTheNoOpAndHidesTransformSliders() {
        val info = info(matched = false)
        compose.setContent { MaterialTheme {
            Column(Modifier.verticalScroll(rememberScrollState())) {
                LensCorrectionEditor(info, controls(info), true, { _, _ -> }, { _, _ -> })
            }
        } }
        compose.onNodeWithText("No calibrated lens profile matches this image.").assertIsDisplayed()
        compose.onNodeWithText("Lens image scale").assertDoesNotExist()
        compose.onNodeWithText("Metadata distortion").assertDoesNotExist()
        compose.onNodeWithContentDescription(context.getString(R.string.lens_profile)).assertIsDisplayed()
    }

    @Test fun busyStateDisablesProfileSelectionDetectionAndParameterChanges() {
        val info = info()
        var edits = 0
        compose.setContent { MaterialTheme {
            Column(Modifier.verticalScroll(rememberScrollState())) {
                LensCorrectionEditor(info, controls(info), false, { _, _ -> edits++ }, { _, _ -> edits++ })
            }
        } }
        compose.onNodeWithContentDescription(context.getString(R.string.lens_profile)).performScrollTo().assertIsNotEnabled()
        compose.onNodeWithText("Detect from image").performScrollTo().assertIsNotEnabled()
        compose.onNodeWithContentDescription(context.getString(R.string.parameter_slider_description, "Vignette strength"))
            .performScrollTo().assertIsNotEnabled()
        compose.runOnIdle { assertEquals(0, edits) }
    }
}
