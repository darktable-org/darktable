/* SPDX-License-Identifier: GPL-3.0-or-later */
package org.example.darktableandroid

import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.MaterialTheme
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.test.assertCountEquals
import androidx.compose.ui.test.assertIsDisplayed
import androidx.compose.ui.test.assertIsEnabled
import androidx.compose.ui.test.assertIsNotEnabled
import androidx.compose.ui.test.junit4.createComposeRule
import androidx.compose.ui.test.onAllNodesWithContentDescription
import androidx.compose.ui.test.onNodeWithContentDescription
import androidx.compose.ui.test.onNodeWithText
import androidx.compose.ui.test.performClick
import androidx.compose.ui.test.performScrollTo
import androidx.compose.ui.test.performTouchInput
import androidx.test.platform.app.InstrumentationRegistry
import org.example.darktableandroid.nativecore.EditorModule
import org.example.darktableandroid.nativecore.ModuleParameter
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Rule
import org.junit.Test

/** Production channel presentation and callbacks; native mask pixels/history have separate checks. */
class ParametricMaskEditorTest {
    @get:Rule val compose = createComposeRule()
    private val context get() = InstrumentationRegistry.getInstrumentation().targetContext

    private fun channel(index: Int, name: String): List<ModuleParameter> =
        listOf(0.1, 0.3, 0.7, 0.9).mapIndexed { handle, value ->
            ModuleParameter("blend.parametric.$index.p$handle", "$name · threshold $handle", "float",
                value.toFloat().toDouble(), 0.0, 1.0, emptyList())
        } + listOf(
            ModuleParameter("blend.parametric.$index.enabled", "$name · enable channel", "bool", 1.0, 0.0, 1.0, emptyList()),
            ModuleParameter("blend.parametric.$index.inverted", "$name · invert channel", "bool", 0.0, 0.0, 1.0, emptyList()),
            ModuleParameter("blend.parametric.$index.reset", "$name · reset channel", "action", 1.0, 1.0, 1.0, emptyList()),
        )

    @Test fun inputOutputSelectionRetainsNativeAddressesAndOneEditPerDrag() {
        val edits = mutableListOf<Pair<String, Double>>()
        val controls = parametricMaskControls(channel(0, "input gray") + channel(4, "output gray"))
        compose.setContent { MaterialTheme {
            Column(Modifier.verticalScroll(rememberScrollState())) {
                ParametricMaskEditor(controls, true) { name, value -> edits += name to value }
            }
        } }
        compose.onNodeWithText(context.getString(R.string.parametric_output)).performScrollTo().performClick()
        val slider = compose.onNodeWithContentDescription(context.getString(R.string.parameter_slider_description,
            "output gray · threshold 1"))
        slider.performScrollTo().performTouchInput {
            down(Offset(width / 3f, centerY))
            moveTo(Offset(width * 0.8f, centerY))
        }
        compose.runOnIdle { assertTrue(edits.isEmpty()) }
        slider.performTouchInput { up() }
        compose.runOnIdle {
            assertEquals(1, edits.size)
            assertEquals("blend.parametric.4.p1", edits.single().first)
            assertTrue(edits.single().second > 0.3 && edits.single().second <= 0.7)
        }
    }

    @Test fun channelMenuAndResetKeepTheOriginalChannelIdentity() {
        val edits = mutableListOf<Pair<String, Double>>()
        val controls = parametricMaskControls(channel(0, "input gray") + channel(1, "input red"))
        compose.setContent { MaterialTheme {
            Column(Modifier.verticalScroll(rememberScrollState())) {
                ParametricMaskEditor(controls, true) { name, value -> edits += name to value }
            }
        } }
        compose.onNodeWithContentDescription(context.getString(R.string.parametric_channel_selector)).performClick()
        compose.onNodeWithText("input red").performClick()
        compose.onNodeWithContentDescription("input red · invert channel").performScrollTo().performClick()
        compose.onNodeWithText("input red · reset channel").performScrollTo().performClick()
        compose.runOnIdle {
            assertEquals(listOf("blend.parametric.1.inverted" to 1.0, "blend.parametric.1.reset" to 1.0), edits)
        }
    }

    @Test fun busyThresholdSliderIsDisabled() {
        compose.setContent { MaterialTheme {
            Column(Modifier.verticalScroll(rememberScrollState())) {
                ParametricMaskEditor(parametricMaskControls(channel(0, "input gray")), false) { _, _ -> }
            }
        } }
        compose.onNodeWithContentDescription(context.getString(R.string.parameter_slider_description,
            "input gray · threshold 0")).performScrollTo().assertIsNotEnabled()
    }

    @Test fun fullSpanKeepsFourSlidersAndNativeNeighborChangesUnlockOuterHandles() {
        val original = channel(0, "input gray").map { parameter ->
            when(parameter.name.substringAfterLast('.')) {
                "p0", "p1", "enabled" -> parameter.copy(value = 0.0)
                "p2", "p3" -> parameter.copy(value = 1.0)
                else -> parameter
            }
        }
        var applied by mutableStateOf(parametricMaskControls(original))
        val edits = mutableListOf<Pair<String, Double>>()
        compose.setContent { MaterialTheme {
            Column(Modifier.verticalScroll(rememberScrollState())) {
                ParametricMaskEditor(applied, true) { name, value -> edits += name to value }
            }
        } }
        fun slider(handle: Int) = compose.onNodeWithContentDescription(context.getString(
            R.string.parameter_slider_description, "input gray · threshold $handle"))
        (0..3).forEach { handle ->
            compose.onAllNodesWithContentDescription(context.getString(R.string.parameter_slider_description,
                "input gray · threshold $handle")).assertCountEquals(1)
            slider(handle).performScrollTo().assertIsDisplayed()
        }
        slider(0).assertIsNotEnabled()
        slider(3).assertIsNotEnabled()
        compose.runOnIdle {
            applied = parametricMaskControls(original.map { parameter ->
                when(parameter.name.substringAfterLast('.')) {
                    "p1" -> parameter.copy(value = 0.25)
                    "p2" -> parameter.copy(value = 0.75)
                    "enabled" -> parameter.copy(value = 1.0)
                    else -> parameter
                }
            })
        }
        slider(0).performScrollTo().assertIsDisplayed().assertIsEnabled()
        slider(3).performScrollTo().assertIsDisplayed().assertIsEnabled()
        compose.runOnIdle { assertTrue("Native metadata refresh must not create another edit", edits.isEmpty()) }
    }

    @Test fun moduleRendersDedicatedThresholdsOnceAndPreservesInstanceSelector() {
        val edits = mutableListOf<Triple<String, String, Double>>()
        val module = EditorModule("exposure@12", "Exposure 12", true, true, channel(0, "input gray"))
        compose.setContent { MaterialTheme {
            Column(Modifier.verticalScroll(rememberScrollState())) {
                ModuleControls(module, true, true, {}, {}, { _, _ -> },
                    { operation, name, value -> edits += Triple(operation, name, value) },
                    { _, _, _, _ -> }, { _, _ -> }, { _, _, _ -> },
                    { _, _ -> }, { _, _ -> }, { _, _, _ -> })
            }
        } }
        compose.onAllNodesWithContentDescription(context.getString(R.string.parameter_slider_description,
            "input gray · threshold 1")).assertCountEquals(1)
        compose.onNodeWithContentDescription("input gray · invert channel").performScrollTo().performClick()
        compose.runOnIdle { assertEquals(listOf(Triple("exposure@12", "blend.parametric.0.inverted", 1.0)), edits) }
    }
}
