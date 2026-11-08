/* SPDX-License-Identifier: GPL-3.0-or-later */
package org.example.darktableandroid

import androidx.compose.material3.MaterialTheme
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import androidx.compose.ui.test.assertIsNotEnabled
import androidx.compose.ui.test.hasSetTextAction
import androidx.compose.ui.test.hasText
import androidx.compose.ui.test.junit4.createComposeRule
import androidx.compose.ui.test.onNodeWithContentDescription
import androidx.compose.ui.test.onNodeWithText
import androidx.compose.ui.test.performClick
import androidx.compose.ui.test.performTextReplacement
import androidx.test.platform.app.InstrumentationRegistry
import org.example.darktableandroid.nativecore.ModuleTextParameter
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Rule
import org.junit.Test

class TextParameterControlTest {
    @get:Rule val compose = createComposeRule()
    private val context get() = InstrumentationRegistry.getInstrumentation().targetContext

    @Test fun editsStayLocalUntilOneValidUtf8Apply() {
        val edits = mutableListOf<String>()
        compose.setContent { MaterialTheme {
            TextParameterControl(ModuleTextParameter("text", "Watermark text", "", 10), true) { edits += it }
        } }
        val input = compose.onNode(hasSetTextAction() and hasText("Watermark text"))
        input.performTextReplacement("相片📷!")
        compose.onNodeWithText(context.getString(R.string.apply)).assertIsNotEnabled()
        compose.runOnIdle { assertTrue(edits.isEmpty()) }
        input.performTextReplacement("相片📷")
        compose.onNodeWithText(context.getString(R.string.apply)).performClick()
        compose.runOnIdle { assertEquals(listOf("相片📷"), edits) }
    }

    @Test fun textResetUsesTheNativeOriginalAndBusyEditorDisablesApply() {
        val edits = mutableListOf<String>()
        var editable by mutableStateOf(true)
        compose.setContent { MaterialTheme {
            TextParameterControl(ModuleTextParameter("text", "Watermark text", "Edited", 32, "Original"), editable) { edits += it }
        } }
        compose.onNodeWithContentDescription(context.getString(R.string.parameter_reset_description, "Watermark text"))
            .performClick()
        compose.runOnIdle { assertEquals(listOf("Original"), edits); editable = false }
        compose.onNodeWithText(context.getString(R.string.apply)).assertIsNotEnabled()
    }
}
