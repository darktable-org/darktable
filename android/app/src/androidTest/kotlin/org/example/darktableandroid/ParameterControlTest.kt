/* SPDX-License-Identifier: GPL-3.0-or-later */
package org.example.darktableandroid

import androidx.compose.material3.MaterialTheme
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.test.assertIsDisplayed
import androidx.compose.ui.test.assertIsNotEnabled
import androidx.compose.ui.test.junit4.createComposeRule
import androidx.compose.ui.test.onNodeWithContentDescription
import androidx.compose.ui.test.onNodeWithText
import androidx.compose.ui.test.performClick
import androidx.compose.ui.test.performTouchInput
import androidx.test.platform.app.InstrumentationRegistry
import org.example.darktableandroid.nativecore.ModuleParameter
import org.example.darktableandroid.nativecore.ParameterChoice
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Rule
import org.junit.Test

class ParameterControlTest {
    @get:Rule val compose = createComposeRule()
    private val context get() = InstrumentationRegistry.getInstrumentation().targetContext
    private fun exposure() = ModuleParameter("exposure", "Exposure", "float", 0.0, -18.0, 18.0,
        emptyList(), sliderMinimum = -3.0, sliderMaximum = 4.0, defaultValue = 0.0)

    @Test fun draggingUpdatesDraftAndCommitsOnceOnRelease() {
        val edits = mutableListOf<Double>()
        compose.setContent { MaterialTheme { ParameterControl(exposure(), true) { edits += it } } }
        val slider = compose.onNodeWithContentDescription(context.getString(R.string.parameter_slider_description, "Exposure"))
        slider.assertIsDisplayed().performTouchInput {
            // The applied zero sits at 3/7 of the upstream soft range.
            down(Offset(width * (3f / 7f), centerY))
            moveTo(Offset(width * 0.8f, centerY))
        }
        compose.runOnIdle { assertTrue("Dragging must not render on every motion", edits.isEmpty()) }
        slider.performTouchInput { up() }
        compose.runOnIdle {
            assertEquals(1, edits.size)
            assertTrue(edits.single() > 0.0 && edits.single() <= 4.0)
        }
    }

    @Test fun resetUsesOriginalDefaultThroughTheEditCallback() {
        var edited: Double? = null
        compose.setContent { MaterialTheme { ParameterControl(exposure().copy(value = 2.0), true) { edited = it } } }
        compose.onNodeWithContentDescription(context.getString(R.string.parameter_reset_description, "Exposure")).performClick()
        compose.runOnIdle { assertEquals(0.0, edited!!, 0.0) }
    }

    @Test fun enumUsesNativeChoicesRatherThanNumericFields() {
        var edited: Double? = null
        val control = ModuleParameter("mode", "Mode", "enum", 2.0, null, null,
            listOf(ParameterChoice(2.0, "First"), ParameterChoice(5.0, "Second")))
        compose.setContent { MaterialTheme { ParameterControl(control, true) { edited = it } } }
        compose.onNodeWithContentDescription("Mode").performClick()
        compose.onNodeWithText("Second").performClick()
        compose.runOnIdle { assertEquals(5.0, edited!!, 0.0) }
    }

    @Test fun booleanSwitchUsesTheNativeBooleanValue() {
        var edited: Double? = null
        compose.setContent { MaterialTheme {
            ParameterControl(ModuleParameter("enabled", "Compensation", "bool", 0.0, 0.0, 1.0, emptyList()), true) { edited = it }
        } }
        compose.onNodeWithContentDescription("Compensation").performClick()
        compose.runOnIdle { assertEquals(1.0, edited!!, 0.0) }
    }

    @Test fun busySliderIsDisabled() {
        compose.setContent { MaterialTheme { ParameterControl(exposure(), false) {} } }
        compose.onNodeWithContentDescription(context.getString(R.string.parameter_slider_description, "Exposure"))
            .assertIsNotEnabled()
    }
}
