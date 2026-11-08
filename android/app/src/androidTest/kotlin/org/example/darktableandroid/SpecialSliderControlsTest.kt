/* SPDX-License-Identifier: GPL-3.0-or-later */
package org.example.darktableandroid

import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.MaterialTheme
import androidx.compose.ui.Modifier
import androidx.compose.ui.semantics.SemanticsActions
import androidx.compose.ui.test.assertIsNotEnabled
import androidx.compose.ui.test.junit4.createComposeRule
import androidx.compose.ui.test.onNodeWithContentDescription
import androidx.compose.ui.test.onNodeWithText
import androidx.compose.ui.test.performClick
import androidx.compose.ui.test.performScrollTo
import androidx.compose.ui.test.performSemanticsAction
import androidx.test.platform.app.InstrumentationRegistry
import org.example.darktableandroid.nativecore.CropRect
import org.example.darktableandroid.nativecore.ModuleCrop
import org.example.darktableandroid.nativecore.WhiteBalance
import org.example.darktableandroid.nativecore.WhiteBalanceChannel
import org.example.darktableandroid.nativecore.WhiteBalanceTemperature
import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Rule
import org.junit.Test

/** Production grouped editors keep native edits atomic while sliders change drafts. */
class SpecialSliderControlsTest {
    @get:Rule val compose = createComposeRule()
    private val context get() = InstrumentationRegistry.getInstrumentation().targetContext

    @Test
    fun temperatureSliderPreservesTintUntilPairIsApplied() {
        var applied: Pair<Double, Double>? = null
        compose.setContent {
            MaterialTheme {
                Column(Modifier.verticalScroll(rememberScrollState())) {
                    WhiteBalanceEditor(balance().copy(temperature = WhiteBalanceTemperature(
                        5000.0, 1.0, 1000.0, 10000.0, 0.1, 10.0)), true, {}, {},
                        { kelvin, tint -> applied = kelvin to tint })
                }
            }
        }
        slider(context.getString(R.string.white_balance_kelvin)).performScrollTo()
            .performSemanticsAction(SemanticsActions.SetProgress) { it(0.75f) }
        compose.runOnIdle { assertNull(applied) }
        compose.onNodeWithText(context.getString(R.string.white_balance_apply_temperature))
            .performScrollTo().performClick()
        compose.runOnIdle { assertEquals(7750.0 to 1.0, applied) }
    }

    @Test
    fun coefficientSliderAppliesAllChannelsInNativeOrder() {
        var applied: FloatArray? = null
        compose.setContent {
            MaterialTheme {
                Column(Modifier.verticalScroll(rememberScrollState())) {
                    WhiteBalanceEditor(balance(), true, { applied = it }, {}, { _, _ -> })
                }
            }
        }
        slider("Red").performScrollTo().performSemanticsAction(SemanticsActions.SetProgress) { it(0.5f) }
        compose.runOnIdle { assertNull(applied) }
        compose.onNodeWithText(context.getString(R.string.white_balance_apply_gains))
            .performScrollTo().performClick()
        compose.runOnIdle { assertArrayEquals(floatArrayOf(2.5f, 1f, 1.5f), applied, 0f) }
    }

    @Test
    fun cropSliderCannotCrossOtherEdgeAndOnlyAppliesCompleteRectangle() {
        var applied: CropRect? = null
        val minimum = 0.1f
        compose.setContent {
            MaterialTheme {
                Column(Modifier.verticalScroll(rememberScrollState())) {
                    CropEditor(ModuleCrop(CropRect(0f, 0f, 1f, 1f), minimum, true), true) { applied = it }
                }
            }
        }
        slider(context.getString(R.string.crop_left)).performScrollTo()
            .performSemanticsAction(SemanticsActions.SetProgress) { it(1f) }
        compose.runOnIdle { assertNull(applied) }
        compose.onNodeWithText(context.getString(R.string.crop_apply)).performScrollTo().performClick()
        compose.runOnIdle {
            val crop = requireNotNull(applied)
            assertTrue(crop.left > 0.8f)
            assertEquals(0f, crop.top, 0f)
            assertEquals(1f, crop.right, 0f)
            assertEquals(1f, crop.bottom, 0f)
            assertTrue(crop.right >= crop.left + minimum)
        }
    }

    @Test
    fun busyEditorDisablesSliderAndApplyTogether() {
        compose.setContent {
            MaterialTheme {
                Column(Modifier.verticalScroll(rememberScrollState())) {
                    WhiteBalanceEditor(balance(), false, {}, {}, { _, _ -> })
                }
            }
        }
        slider("Red").performScrollTo().assertIsNotEnabled()
        compose.onNodeWithText(context.getString(R.string.white_balance_apply_gains))
            .performScrollTo().assertIsNotEnabled()
    }

    private fun slider(label: String) = compose.onNodeWithContentDescription(
        context.getString(R.string.parameter_slider_description, label))

    private fun balance() = WhiteBalance(
        // Intentionally use a display order different from native coefficient order.
        channels = listOf(WhiteBalanceChannel(2, "Blue", 1.5f),
            WhiteBalanceChannel(0, "Red", 2f), WhiteBalanceChannel(1, "Green", 1f)),
        manualMin = 1f, manualMax = 4f, setting = 2,
        settingLabel = "User defined", settings = emptyList(),
    )
}
