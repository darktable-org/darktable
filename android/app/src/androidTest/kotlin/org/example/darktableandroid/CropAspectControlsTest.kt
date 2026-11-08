/* SPDX-License-Identifier: GPL-3.0-or-later */
package org.example.darktableandroid

import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.MaterialTheme
import androidx.compose.ui.Modifier
import androidx.compose.ui.test.assertIsNotEnabled
import androidx.compose.ui.test.junit4.createComposeRule
import androidx.compose.ui.test.onNodeWithContentDescription
import androidx.compose.ui.test.onNodeWithText
import androidx.compose.ui.test.performClick
import androidx.compose.ui.test.performScrollTo
import androidx.test.platform.app.InstrumentationRegistry
import org.example.darktableandroid.nativecore.CropRect
import org.example.darktableandroid.nativecore.ModuleCrop
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Rule
import org.junit.Test

/** The production ratio picker changes a complete draft; Apply is its only native edit. */
class CropAspectControlsTest {
    @get:Rule val compose = createComposeRule()
    private val context get() = InstrumentationRegistry.getInstrumentation().targetContext

    @Test fun squareDraftUsesNativeInputDimensionsAndOneApply() {
        var applied: CropRect? = null
        show { applied = it }
        ratioPicker().performScrollTo().performClick()
        compose.onNodeWithText("1:1").performClick()
        compose.runOnIdle { assertNull(applied) }
        apply().performScrollTo().performClick()
        compose.runOnIdle {
            val rect = requireNotNull(applied)
            assertEquals(1f / 6f, rect.left, 0.000001f)
            assertEquals(5f / 6f, rect.right, 0.000001f)
            assertEquals(0f, rect.top, 0f)
            assertEquals(1f, rect.bottom, 0f)
        }
    }

    @Test fun portraitOrientationRemainsADraftUntilApply() {
        var applied: CropRect? = null
        show { applied = it }
        ratioPicker().performScrollTo().performClick()
        compose.onNodeWithText("3:2").performClick()
        val orientation = compose.onNodeWithContentDescription(context.getString(R.string.crop_aspect_orientation))
        orientation.performScrollTo().performClick()
        compose.runOnIdle { assertNull(applied) }
        apply().performScrollTo().performClick()
        compose.runOnIdle {
            val rect = requireNotNull(applied)
            assertEquals(2.0 / 3.0, (rect.right - rect.left).toDouble() * 6000 /
                ((rect.bottom - rect.top).toDouble() * 4000), 0.00001)
        }
    }

    @Test fun oldRuntimeOmitsRatioPicker() {
        compose.setContent {
            MaterialTheme {
                Column(Modifier.verticalScroll(rememberScrollState())) {
                    CropEditor(ModuleCrop(CropRect(0f, 0f, 1f, 1f), 0.01f, true), true) {}
                }
            }
        }
        ratioPicker().assertDoesNotExist()
    }

    @Test fun busyRuntimeDisablesRatioAndOrientationTogether() {
        show(editable = false) {}
        ratioPicker().performScrollTo().assertIsNotEnabled()
        compose.onNodeWithContentDescription(context.getString(R.string.crop_aspect_orientation))
            .performScrollTo().assertIsNotEnabled()
    }

    private fun show(editable: Boolean = true, onApply: (CropRect) -> Unit) {
        compose.setContent {
            MaterialTheme {
                Column(Modifier.verticalScroll(rememberScrollState())) {
                    CropEditor(ModuleCrop(CropRect(0f, 0f, 1f, 1f), 0.01f, true, 6000, 4000), editable, onApply)
                }
            }
        }
    }

    private fun ratioPicker() = compose.onNodeWithContentDescription(context.getString(R.string.crop_aspect_title))
    private fun apply() = compose.onNodeWithText(context.getString(R.string.crop_apply))
}
