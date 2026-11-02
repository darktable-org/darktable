/* SPDX-License-Identifier: GPL-3.0-or-later */
package org.example.darktableandroid

import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.MaterialTheme
import androidx.compose.runtime.mutableStateOf
import androidx.compose.ui.Modifier
import androidx.compose.ui.semantics.SemanticsActions
import androidx.compose.ui.semantics.SemanticsProperties
import androidx.compose.ui.test.SemanticsMatcher
import androidx.compose.ui.test.assert
import androidx.compose.ui.test.assertIsDisplayed
import androidx.compose.ui.test.assertIsNotEnabled
import androidx.compose.ui.test.junit4.createComposeRule
import androidx.compose.ui.test.onNodeWithContentDescription
import androidx.compose.ui.test.onNodeWithText
import androidx.compose.ui.test.performClick
import androidx.compose.ui.test.performScrollTo
import androidx.compose.ui.test.performSemanticsAction
import androidx.test.platform.app.InstrumentationRegistry
import org.example.darktableandroid.nativecore.WhiteBalance
import org.example.darktableandroid.nativecore.WhiteBalanceCameraPresets
import org.example.darktableandroid.nativecore.WhiteBalanceChannel
import org.example.darktableandroid.nativecore.WhiteBalancePreset
import org.example.darktableandroid.nativecore.WhiteBalanceSetting
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Rule
import org.junit.Test

/** The native edit stays atomic even when the user browses or fine-tunes a draft. */
class WhiteBalancePresetEditorTest {
    @get:Rule val compose = createComposeRule()
    private val context get() = InstrumentationRegistry.getInstrumentation().targetContext
    private val presets = listOf(WhiteBalancePreset(6, "Daylight", listOf(-4, 0, 3), 0),
        WhiteBalancePreset(9, "Shade", listOf(-7, 2, 8), 2))
    private fun balance(preset: Int? = 6, tuning: Int? = -4) = WhiteBalance(
        listOf(WhiteBalanceChannel(0, "red", 2f), WhiteBalanceChannel(1, "green", 1f),
            WhiteBalanceChannel(2, "blue", 1.5f)), 0.001f, 8f, preset ?: 2,
        presets.singleOrNull { it.id == preset }?.label ?: "user", listOf(WhiteBalanceSetting(0, "as shot")),
        cameraPresets = WhiteBalanceCameraPresets("Native Camera", presets, preset, tuning))

    @Test fun browsingAndSparseFineTuningEmitOneExactPresetPairOnlyOnApply() {
        val edits = mutableListOf<Pair<Int, Int>>()
        var otherEdits = 0
        compose.setContent { MaterialTheme {
            Column(Modifier.verticalScroll(rememberScrollState())) {
                WhiteBalanceEditor(balance().copy(settingLabel = "user defined"), true,
                    { otherEdits++ }, { otherEdits++ }, { _, _ -> otherEdits++ },
                    { preset, tuning -> edits += preset to tuning })
            }
        } }
        compose.onNodeWithContentDescription(context.getString(R.string.white_balance_camera_preset))
            .performScrollTo().performClick()
        compose.onNodeWithText("Shade").performClick()
        compose.onNodeWithText(context.getString(R.string.white_balance_tuning_value, 2)).assertIsDisplayed()
        compose.onNodeWithContentDescription(context.getString(R.string.white_balance_fine_tune))
            .performScrollTo().performSemanticsAction(SemanticsActions.SetProgress) { it(2f) }
        compose.onNodeWithText(context.getString(R.string.white_balance_current, "Daylight"))
            .performScrollTo().assertIsDisplayed()
        compose.onNodeWithText(context.getString(R.string.white_balance_current, "user defined")).assertDoesNotExist()
        compose.runOnIdle { assertTrue(edits.isEmpty()); assertEquals(0, otherEdits) }
        compose.onNodeWithText(context.getString(R.string.white_balance_apply_preset)).performScrollTo().performClick()
        compose.runOnIdle { assertEquals(listOf(9 to 8), edits); assertEquals(0, otherEdits) }
    }

    @Test fun undoMetadataRestoresTheAppliedPresetAndTuningAndClearsPendingControls() {
        val current = mutableStateOf(balance())
        val edits = mutableListOf<Pair<Int, Int>>()
        compose.setContent { MaterialTheme {
            Column(Modifier.verticalScroll(rememberScrollState())) {
                WhiteBalanceEditor(current.value, true, {}, {}, { _, _ -> }, { preset, tuning -> edits += preset to tuning })
            }
        } }
        compose.onNodeWithContentDescription(context.getString(R.string.white_balance_fine_tune))
            .performScrollTo().performSemanticsAction(SemanticsActions.SetProgress) { it(2f) }
        compose.runOnIdle { current.value = balance(9, 8) }
        compose.onNodeWithText("Shade").performScrollTo().assertIsDisplayed()
        compose.onNodeWithContentDescription(context.getString(R.string.white_balance_fine_tune))
            .assert(SemanticsMatcher.expectValue(SemanticsProperties.StateDescription, "8"))
        compose.runOnIdle { current.value = balance(6, -4) }
        compose.onNodeWithText("Daylight").performScrollTo().assertIsDisplayed()
        compose.onNodeWithContentDescription(context.getString(R.string.white_balance_fine_tune))
            .assert(SemanticsMatcher.expectValue(SemanticsProperties.StateDescription, "-4"))
        compose.onNodeWithText(context.getString(R.string.white_balance_current_preset, "Daylight", -4))
            .performScrollTo().assertIsDisplayed()
        compose.onNodeWithText(context.getString(R.string.white_balance_apply_preset)).performScrollTo().assertIsNotEnabled()
        compose.runOnIdle { assertTrue(edits.isEmpty()) }
    }

    @Test fun unavailableCatalogKeepsCoefficientsWithoutAnEmptyPresetPicker() {
        compose.setContent { MaterialTheme {
            Column(Modifier.verticalScroll(rememberScrollState())) {
                WhiteBalanceEditor(balance().copy(cameraPresets = null, settingLabel = "user defined"),
                    true, {}, {}, { _, _ -> })
            }
        } }
        compose.onNodeWithContentDescription(context.getString(R.string.white_balance_camera_preset)).assertDoesNotExist()
        compose.onNodeWithContentDescription(context.getString(R.string.white_balance_fine_tune)).assertDoesNotExist()
        compose.onNodeWithText(context.getString(R.string.white_balance_current, "user defined"))
            .performScrollTo().assertIsDisplayed()
        compose.onNodeWithText(context.getString(R.string.white_balance_apply_gains)).performScrollTo().assertIsDisplayed()
    }

    @Test fun aSingleNativeTuningDoesNotOfferASlider() {
        val fixed = balance(null, null).copy(cameraPresets = WhiteBalanceCameraPresets("Fixed Camera",
            listOf(WhiteBalancePreset(12, "Flash", listOf(4), 4)), null, null))
        val edits = mutableListOf<Pair<Int, Int>>()
        compose.setContent { MaterialTheme {
            Column(Modifier.verticalScroll(rememberScrollState())) {
                WhiteBalanceEditor(fixed, true, {}, {}, { _, _ -> }, { preset, tuning -> edits += preset to tuning })
            }
        } }
        compose.onNodeWithContentDescription(context.getString(R.string.white_balance_fine_tune)).assertDoesNotExist()
        compose.onNodeWithText(context.getString(R.string.white_balance_tuning_value, 4)).performScrollTo().assertIsDisplayed()
        compose.onNodeWithText(context.getString(R.string.white_balance_apply_preset)).performScrollTo().performClick()
        compose.runOnIdle { assertEquals(listOf(12 to 4), edits) }
    }

    @Test fun busyStateDisablesPresetSelectionTuningAndApply() {
        compose.setContent { MaterialTheme {
            Column(Modifier.verticalScroll(rememberScrollState())) {
                WhiteBalanceEditor(balance(null, null), false, {}, {}, { _, _ -> })
            }
        } }
        compose.onNodeWithContentDescription(context.getString(R.string.white_balance_camera_preset))
            .performScrollTo().assertIsNotEnabled()
        compose.onNodeWithContentDescription(context.getString(R.string.white_balance_fine_tune))
            .performScrollTo().assertIsNotEnabled()
        compose.onNodeWithText(context.getString(R.string.white_balance_apply_preset)).performScrollTo().assertIsNotEnabled()
    }
}
