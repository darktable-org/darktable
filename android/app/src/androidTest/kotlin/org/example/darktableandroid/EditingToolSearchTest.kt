/* SPDX-License-Identifier: GPL-3.0-or-later */
package org.example.darktableandroid

import androidx.compose.material3.MaterialTheme
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import androidx.compose.ui.test.assertIsDisplayed
import androidx.compose.ui.test.junit4.createComposeRule
import androidx.compose.ui.test.onNodeWithText
import androidx.compose.ui.test.performClick
import androidx.compose.ui.test.performTextReplacement
import androidx.test.platform.app.InstrumentationRegistry
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Rule
import org.junit.Test

class EditingToolSearchTest {
    @get:Rule val compose = createComposeRule()
    private val context get() = InstrumentationRegistry.getInstrumentation().targetContext

    @Test fun searchClearAndEnabledFilterUseProductionCallbacks() {
        var query by mutableStateOf("")
        var enabledOnly by mutableStateOf(false)
        compose.setContent { MaterialTheme {
            EditingToolSearch(query, { query = it }, enabledOnly, { enabledOnly = it }, 12, 80)
        } }
        compose.onNodeWithText(context.getString(R.string.tools_search)).performTextReplacement("contrast")
        compose.runOnIdle { assertEquals("contrast", query) }
        compose.onNodeWithText(context.getString(R.string.tools_clear_search)).performClick()
        compose.runOnIdle { assertEquals("", query) }
        compose.onNodeWithText(context.getString(R.string.tools_enabled)).performClick()
        compose.runOnIdle { assertTrue(enabledOnly) }
        compose.onNodeWithText(context.getString(R.string.tools_visible_count, 12, 80)).assertIsDisplayed()
    }
}
