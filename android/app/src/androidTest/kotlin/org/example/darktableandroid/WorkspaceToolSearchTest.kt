/* SPDX-License-Identifier: GPL-3.0-or-later */
package org.example.darktableandroid

import android.graphics.Bitmap
import android.net.Uri
import androidx.compose.foundation.layout.Column
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import androidx.compose.ui.test.assertIsDisplayed
import androidx.compose.ui.test.assertIsNotEnabled
import androidx.compose.ui.test.assertIsSelected
import androidx.compose.ui.test.assertTextEquals
import androidx.compose.ui.test.hasTestTag
import androidx.compose.ui.test.junit4.createComposeRule
import androidx.compose.ui.test.onNodeWithTag
import androidx.compose.ui.test.onNodeWithText
import androidx.compose.ui.test.performClick
import androidx.compose.ui.test.performScrollToNode
import androidx.compose.ui.test.performTextReplacement
import androidx.test.platform.app.InstrumentationRegistry
import org.example.darktableandroid.nativecore.EditorModule
import org.example.darktableandroid.nativecore.ModuleParameter
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Rule
import org.junit.Test

/** Production search navigation; this bitmap/callback fixture does not render the native engine. */
class WorkspaceToolSearchTest {
    @get:Rule val compose = createComposeRule()
    private val context get() = InstrumentationRegistry.getInstrumentation().targetContext

    private fun module(operation: String, name: String, enabled: Boolean = true) =
        EditorModule(operation, name, enabled, true, listOf(
            ModuleParameter("radius", "Radius", "float", 0.5, 0.0, 1.0, emptyList()),
            ModuleParameter("amount", "Amount", "float", 0.5, 0.0, 1.0, emptyList()),
        ))

    private fun ready(selected: String = "exposure") = EditorState.Ready(
        Uri.parse("content://workspace-search-fixture/photo.raw"), 77L,
        Bitmap.createBitmap(32, 24, Bitmap.Config.ARGB_8888),
        listOf(module("exposure", "Exposure"), module("exposure@7", "Exposure", false),
            module("sharpen", "Sharpen"), module("crop", "Crop")),
        emptyList(), selectedModule = selected,
    )

    @Composable
    private fun Fixture(
        state: EditorState,
        onSelect: (String) -> Unit,
        onEnabled: (String, Boolean) -> Unit = { _, _ -> },
    ) {
        MaterialTheme {
            EditorWorkspace(state, RuntimeAvailability(checking = false, available = true),
                {}, {}, {}, {}, {}, {}, onSelect, onEnabled) { module, query ->
                Column {
                    Text("Parameters for ${module.operation}")
                    Text("Editor query: [$query]")
                    module.parameters.forEach { Text(it.label) }
                }
            }
        }
    }

    private fun search(query: String) {
        compose.onNodeWithTag("editor_tool_search_button").performClick()
        compose.onNodeWithTag("editor_tool_search_input").performTextReplacement(query)
    }

    @Test fun typingShowsResultsFromOtherCategoriesWithoutSelectingThem() {
        var current by mutableStateOf(ready())
        val selections = mutableListOf<String>()
        compose.setContent { Fixture(current, { operation ->
            selections += operation
            current = current.copy(selectedModule = operation)
        }) }

        search("sharpen")

        compose.onNodeWithTag("editor_tool_search_results").assertIsDisplayed()
        compose.onNodeWithTag("editor_tool_search_count")
            .assertTextEquals(context.getString(R.string.workspace_search_results, 1))
        compose.onNodeWithTag("editor_tool_search_result_sharpen").assertIsDisplayed()
        compose.onNodeWithTag("editor_tool_search_result_exposure").assertDoesNotExist()
        compose.onNodeWithText(context.getString(R.string.workspace_tab_adjustments)).assertIsSelected()
        compose.runOnIdle {
            assertEquals("exposure", current.selectedModule)
            assertTrue("Entering a query must not select its first match", selections.isEmpty())
        }
    }

    @Test fun choosingCrossCategoryResultOpensTheWholeToolAndRemembersThePreviousTool() {
        var current by mutableStateOf(ready("exposure@7"))
        val enabled = mutableListOf<Pair<String, Boolean>>()
        compose.setContent { Fixture(current, { current = current.copy(selectedModule = it) },
            { operation, value -> enabled += operation to value }) }

        search("sharpen radius")
        compose.onNodeWithTag("editor_tool_search_result_sharpen").performClick()

        compose.onNodeWithText(context.getString(R.string.workspace_tab_detail)).assertIsSelected()
        compose.onNodeWithText("Parameters for sharpen").assertIsDisplayed()
        compose.onNodeWithText("Amount").assertIsDisplayed()
        compose.onNodeWithText("Editor query: []").assertIsDisplayed()
        compose.onNodeWithTag("editor_tool_search_results").assertDoesNotExist()
        compose.onNodeWithTag("editor_tool_search_input").assertDoesNotExist()
        compose.onNodeWithText(context.getString(R.string.workspace_tab_adjustments)).performClick()
        compose.onNodeWithText("Parameters for exposure@7").assertIsDisplayed()
        compose.runOnIdle { assertTrue("Search navigation must not enable any tool", enabled.isEmpty()) }
    }

    @Test fun duplicateNamesSelectTheExactDisabledInstanceWithoutTurningItOn() {
        var current by mutableStateOf(ready())
        val selections = mutableListOf<String>()
        val enabled = mutableListOf<Pair<String, Boolean>>()
        compose.setContent { Fixture(current, { operation ->
            selections += operation
            current = current.copy(selectedModule = operation)
        }, { operation, value -> enabled += operation to value }) }

        search("exposure")
        compose.onNodeWithTag("editor_tool_search_count")
            .assertTextEquals(context.getString(R.string.workspace_search_results, 2))
        compose.onNodeWithTag("editor_tool_search_result_exposure").assertIsDisplayed()
        compose.onNodeWithTag("editor_tool_search_results")
            .performScrollToNode(hasTestTag("editor_tool_search_result_exposure@7"))
        compose.onNodeWithTag("editor_tool_search_result_exposure@7").performClick()

        compose.onNodeWithText("Parameters for exposure@7").assertIsDisplayed()
        compose.runOnIdle {
            assertEquals(listOf("exposure@7"), selections)
            assertEquals("exposure@7", current.selectedModule)
            assertFalse(current.modules.single { it.operation == "exposure@7" }.enabled)
            assertTrue(current.modules.single { it.operation == "exposure" }.enabled)
            assertTrue(enabled.isEmpty())
        }
    }

    @Test fun noMatchesAndClosingSearchKeepTheCurrentToolAndCategory() {
        var current by mutableStateOf(ready())
        val selections = mutableListOf<String>()
        compose.setContent { Fixture(current, { operation ->
            selections += operation
            current = current.copy(selectedModule = operation)
        }) }
        compose.onNodeWithText(context.getString(R.string.workspace_tab_geometry)).performClick()
        compose.onNodeWithText("Parameters for crop").assertIsDisplayed()
        compose.runOnIdle { selections.clear() }

        search("no_such_tool_129")
        compose.onNodeWithText(context.getString(R.string.tools_no_matches)).assertIsDisplayed()
        compose.onNodeWithTag("editor_tool_search_button").performClick()

        compose.onNodeWithTag("editor_tool_search_results").assertDoesNotExist()
        compose.onNodeWithText(context.getString(R.string.workspace_tab_geometry)).assertIsSelected()
        compose.onNodeWithText("Parameters for crop").assertIsDisplayed()
        compose.runOnIdle {
            assertEquals("crop", current.selectedModule)
            assertTrue("A failed or cancelled search must not change selection", selections.isEmpty())
        }
    }

    @Test fun enabledFilterOnlyChangesResultsAndCancelRetainsTheDisabledTool() {
        var current by mutableStateOf(ready("exposure@7"))
        val selections = mutableListOf<String>()
        compose.setContent { Fixture(current, { operation ->
            selections += operation
            current = current.copy(selectedModule = operation)
        }) }

        search("exposure")
        compose.onNodeWithTag("editor_tool_search_enabled").performClick()
        compose.onNodeWithTag("editor_tool_search_count")
            .assertTextEquals(context.getString(R.string.workspace_search_results, 1))
        compose.onNodeWithTag("editor_tool_search_result_exposure").assertIsDisplayed()
        compose.onNodeWithTag("editor_tool_search_result_exposure@7").assertDoesNotExist()
        compose.runOnIdle {
            assertEquals("exposure@7", current.selectedModule)
            assertTrue(selections.isEmpty())
        }
        compose.onNodeWithTag("editor_tool_search_button").performClick()
        compose.onNodeWithText("Parameters for exposure@7").assertIsDisplayed()
    }

    @Test fun resultsCannotNavigateWhileAnEditIsRunning() {
        var current: EditorState by mutableStateOf(ready())
        val selections = mutableListOf<String>()
        compose.setContent { Fixture(current, { selections += it }) }
        search("sharpen")
        compose.runOnIdle { current = EditorState.Working(EditorState.Stage.EDITING, current as EditorState.Ready) }

        compose.onNodeWithTag("editor_tool_search_result_sharpen").assertIsNotEnabled().performClick()
        compose.runOnIdle { assertTrue("An in-flight edit must retain its native tool", selections.isEmpty()) }
    }
}
