/* SPDX-License-Identifier: GPL-3.0-or-later */
package org.example.darktableandroid

import androidx.compose.material3.MaterialTheme
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import androidx.compose.ui.test.assertIsDisplayed
import androidx.compose.ui.test.assertIsOn
import androidx.compose.ui.test.hasSetTextAction
import androidx.compose.ui.test.hasText
import androidx.compose.ui.test.isToggleable
import androidx.compose.ui.test.junit4.createComposeRule
import androidx.compose.ui.test.onNodeWithContentDescription
import androidx.compose.ui.test.onNodeWithText
import androidx.compose.ui.test.performClick
import androidx.compose.ui.test.performTextReplacement
import androidx.test.platform.app.InstrumentationRegistry
import org.example.darktableandroid.nativecore.EditorModule
import org.example.darktableandroid.nativecore.ModuleParameter
import org.junit.Assert.assertEquals
import org.junit.Rule
import org.junit.Test

/** Actual Compose interactions with production controls and callback fixtures.
 * These tests establish UI behavior; native pixel/undo validation is separate. */
class ModuleControlsTest {
    @get:Rule val compose = createComposeRule()
    private val context get() = InstrumentationRegistry.getInstrumentation().targetContext

    @Test
    fun expandEditorThenApplyNumericParameter() {
        val initial = exposure()
        var module by mutableStateOf(initial)
        var selected by mutableStateOf(false)
        var edited: Triple<String, String, Double>? = null
        compose.setContent {
            MaterialTheme {
                ModuleControls(module, true, selected,
                    { selected = it == module.operation }, {},
                    { _, enabled -> module = module.copy(enabled = enabled) },
                    { operation, parameter, value -> edited = Triple(operation, parameter, value) },
                    { _, _, _, _ -> }, { _, _ -> }, { _, _, _ -> },
                    { _, _ -> }, { _, _ -> }, { _, _, _ -> })
            }
        }
        compose.onNodeWithText("Exposure value").assertDoesNotExist()
        compose.onNodeWithContentDescription(context.getString(R.string.module_expand_controls, initial.name))
            .performClick()
        compose.onNode(hasSetTextAction() and hasText("Exposure value"))
            .assertIsDisplayed().performTextReplacement("-1.25")
        compose.onNodeWithText(context.getString(R.string.apply)).performClick()
        compose.runOnIdle { assertEquals(Triple("exposure", "exposure", -1.25), edited) }
        compose.onNodeWithContentDescription(context.getString(R.string.module_collapse_controls, initial.name))
            .performClick()
        compose.onNodeWithText("Exposure value").assertDoesNotExist()
    }

    @Test
    fun enablingModuleRevealsItsAdjustments() {
        var module by mutableStateOf(exposure())
        var selected by mutableStateOf(false)
        var selectedOperation: String? = null
        var enabledOperation: String? = null
        compose.setContent {
            MaterialTheme {
                ModuleControls(module, true, selected,
                    { selectedOperation = it; selected = it == module.operation }, {},
                    { operation, enabled -> enabledOperation = operation; module = module.copy(enabled = enabled) },
                    { _, _, _ -> }, { _, _, _, _ -> }, { _, _ -> }, { _, _, _ -> },
                    { _, _ -> }, { _, _ -> }, { _, _, _ -> })
            }
        }
        compose.onNodeWithText("Exposure value").assertDoesNotExist()
        compose.onNode(isToggleable()).performClick().assertIsOn()
        compose.onNode(hasSetTextAction() and hasText("Exposure value")).assertIsDisplayed()
        compose.runOnIdle {
            assertEquals("exposure", selectedOperation)
            assertEquals("exposure", enabledOperation)
        }
    }

    @Test
    fun unsupportedToolExplainsMissingControls() {
        val module = exposure().copy(operation = "unavailable", name = "Unavailable tool", parameters = emptyList())
        var selected by mutableStateOf(false)
        compose.setContent {
            MaterialTheme {
                ModuleControls(module, true, selected,
                    { selected = it == module.operation }, {}, { _, _ -> },
                    { _, _, _ -> }, { _, _, _, _ -> }, { _, _ -> }, { _, _, _ -> },
                    { _, _ -> }, { _, _ -> }, { _, _, _ -> })
            }
        }
        compose.onNodeWithContentDescription(context.getString(R.string.module_expand_controls, module.name))
            .performClick()
        compose.onNodeWithText(context.getString(R.string.no_numeric_parameters)).assertIsDisplayed()
    }

    private fun exposure() = EditorModule(
        operation = "exposure", name = "Exposure", enabled = false, canDisable = true,
        parameters = listOf(ModuleParameter("exposure", "Exposure value", "float", 0.0, -18.0, 18.0, emptyList())),
    )
}
