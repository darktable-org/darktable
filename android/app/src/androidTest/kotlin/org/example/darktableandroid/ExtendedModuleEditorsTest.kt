/* SPDX-License-Identifier: GPL-3.0-or-later */
package org.example.darktableandroid

import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.MaterialTheme
import androidx.compose.ui.Modifier
import androidx.compose.ui.semantics.SemanticsActions
import androidx.compose.ui.test.assertIsDisplayed
import androidx.compose.ui.test.junit4.createComposeRule
import androidx.compose.ui.test.onNodeWithContentDescription
import androidx.compose.ui.test.onNodeWithText
import androidx.compose.ui.test.performClick
import androidx.compose.ui.test.performScrollTo
import androidx.compose.ui.test.performSemanticsAction
import androidx.test.platform.app.InstrumentationRegistry
import org.example.darktableandroid.nativecore.CurvePoint
import org.example.darktableandroid.nativecore.EditorModule
import org.example.darktableandroid.nativecore.ModuleCurve
import org.example.darktableandroid.nativecore.ModuleParameter
import org.example.darktableandroid.nativecore.ParameterChoice
import org.junit.Assert.assertEquals
import org.junit.Rule
import org.junit.Test

/** Real production UI callbacks; native blending pixels/history are checked separately. */
class ExtendedModuleEditorsTest {
    @get:Rule val compose = createComposeRule()
    private val context get() = InstrumentationRegistry.getInstrumentation().targetContext

    @Test fun blendingControlsRetainTheExactModuleInstanceAndNativeChoiceValues() {
        val edits = mutableListOf<Triple<String, String, Double>>()
        val module = EditorModule("exposure@7", "Exposure 7", true, true, listOf(
            ModuleParameter("blend.opacity", "Opacity", "float", 100.0, 0.0, 100.0, emptyList()),
            ModuleParameter("blend.mode", "Blend mode", "enum", 1.0, null, null,
                listOf(ParameterChoice(1.0, "Normal"), ParameterChoice(9.0, "Multiply"))),
            ModuleParameter("blend.enabled", "Enable blending", "bool", 0.0, 0.0, 1.0, emptyList()),
        ), canReset = true)
        compose.setContent { MaterialTheme {
            Column(Modifier.verticalScroll(rememberScrollState())) {
                ModuleControls(module, true, true, {}, {}, { _, _ -> },
                    { operation, parameter, value -> edits += Triple(operation, parameter, value) },
                    { _, _, _, _ -> }, { _, _ -> }, { _, _, _ -> },
                    { _, _ -> }, { _, _ -> }, { _, _, _ -> })
            }
        } }
        compose.onNodeWithText(context.getString(R.string.module_blending)).performScrollTo().assertIsDisplayed()
        compose.onNodeWithContentDescription(context.getString(R.string.parameter_slider_description, "Opacity"))
            .performScrollTo().performSemanticsAction(SemanticsActions.SetProgress) { it(0.4f) }
        compose.onNodeWithContentDescription("Blend mode").performScrollTo().performClick()
        compose.onNodeWithText("Multiply").performClick()
        compose.onNodeWithContentDescription("Enable blending").performScrollTo().performClick()
        compose.onNodeWithContentDescription(context.getString(R.string.module_reset_description, module.name))
            .performScrollTo().performClick()
        compose.runOnIdle {
            assertEquals(4, edits.size)
            assertEquals("exposure@7", edits[0].first)
            assertEquals("blend.opacity", edits[0].second)
            assertEquals(40.0, edits[0].third, 0.00001)
            assertEquals(Triple("exposure@7", "blend.mode", 9.0), edits[1])
            assertEquals(Triple("exposure@7", "blend.enabled", 1.0), edits[2])
            assertEquals(Triple("exposure@7", "module.reset", 1.0), edits[3])
        }
    }

    @Test fun colorZonesResetAppliesTheNativeNeutralCurveRatherThanADiagonal() {
        val neutral = listOf(CurvePoint(0f, 0.5f), CurvePoint(1f, 0.5f))
        val curve = ModuleCurve(1, "Chroma", true, 1, 20, 0.000001f,
            listOf(CurvePoint(0f, 0.2f), CurvePoint(1f, 0.8f)),
            listOf(CurvePoint(0f, 0.2f), CurvePoint(1f, 0.8f)), resetNodes = neutral)
        var result: Triple<Int, Int, List<CurvePoint>>? = null
        compose.setContent { MaterialTheme {
            Column(Modifier.verticalScroll(rememberScrollState())) {
                ModuleCurves(listOf(curve), true) { channel, interpolation, nodes ->
                    result = Triple(channel, interpolation, nodes)
                }
            }
        } }
        compose.onNodeWithText(context.getString(R.string.curve_reset)).performScrollTo().performClick()
        compose.onNodeWithText(context.getString(R.string.curve_apply)).performScrollTo().performClick()
        compose.runOnIdle { assertEquals(Triple(1, 1, neutral), result) }
    }

    @Test fun drawnMaskActionsUseButtonsAndOneNativeActionValue() {
        val edits = mutableListOf<Triple<String, String, Double>>()
        val action = ModuleParameter("mask.add.circle", "add circle mask", "action", 1.0, 0.0, 1.0, emptyList(), defaultValue = 0.0)
        val module = EditorModule("exposure@9", "Exposure 9", true, true, listOf(action))
        compose.setContent { MaterialTheme {
            Column(Modifier.verticalScroll(rememberScrollState())) {
                ModuleControls(module, true, true, {}, {}, { _, _ -> },
                    { operation, parameter, value -> edits += Triple(operation, parameter, value) },
                    { _, _, _, _ -> }, { _, _ -> }, { _, _, _ -> },
                    { _, _ -> }, { _, _ -> }, { _, _, _ -> })
            }
        } }
        compose.onNodeWithText(context.getString(R.string.module_drawn_masks)).performScrollTo().assertIsDisplayed()
        compose.onNodeWithText("add circle mask").performScrollTo().performClick()
        compose.onNodeWithContentDescription(context.getString(R.string.parameter_slider_description, "add circle mask"))
            .assertDoesNotExist()
        compose.onNodeWithContentDescription(context.getString(R.string.parameter_reset_description, "add circle mask"))
            .assertDoesNotExist()
        compose.runOnIdle { assertEquals(listOf(Triple("exposure@9", "mask.add.circle", 1.0)), edits) }
    }
}
