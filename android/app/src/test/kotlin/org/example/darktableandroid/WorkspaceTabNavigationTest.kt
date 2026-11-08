/* SPDX-License-Identifier: GPL-3.0-or-later */
package org.example.darktableandroid

import android.graphics.Bitmap
import android.content.pm.ActivityInfo
import android.net.Uri
import androidx.activity.ComponentActivity
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import androidx.compose.ui.test.assertIsDisplayed
import androidx.compose.ui.test.assertIsSelected
import androidx.compose.ui.test.junit4.createComposeRule
import androidx.compose.ui.test.onNodeWithTag
import androidx.compose.ui.test.onNodeWithText
import androidx.compose.ui.test.performClick
import androidx.compose.ui.test.performTextReplacement
import org.example.darktableandroid.nativecore.EditorModule
import org.example.darktableandroid.nativecore.ModuleParameter
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Rule
import org.junit.Test
import org.junit.runner.RunWith
import org.junit.rules.ExternalResource
import org.junit.rules.RuleChain
import org.junit.rules.TestRule
import org.robolectric.RobolectricTestRunner
import org.robolectric.RuntimeEnvironment
import org.robolectric.Shadows.shadowOf
import org.robolectric.annotation.Config
import org.robolectric.annotation.GraphicsMode

/** Executes the production Compose workspace on a host Android runtime, without native image processing. */
@RunWith(RobolectricTestRunner::class)
@Config(sdk = [35], qualifiers = "w400dp-h800dp-mdpi")
@GraphicsMode(GraphicsMode.Mode.NATIVE)
class WorkspaceTabNavigationTest {
    private val compose = createComposeRule()
    private val context: android.content.Context get() = RuntimeEnvironment.getApplication()
    // Release unit tests do not merge debugImplementation's test-activity
    // manifest. Register only inside Robolectric, before Compose launches it.
    @get:Rule val rules: TestRule = RuleChain.outerRule(object : ExternalResource() {
        override fun before() {
            shadowOf(context.packageManager).addOrUpdateActivity(ActivityInfo().apply {
                name = ComponentActivity::class.java.name
                packageName = context.packageName
                applicationInfo = context.applicationInfo
                theme = android.R.style.Theme_Material_Light_NoActionBar
                enabled = true
                exported = true
            })
        }
    }).around(compose)

    private fun module(operation: String, name: String, enabled: Boolean = true) =
        EditorModule(operation, name, enabled, true, listOf(
            ModuleParameter("amount", "Amount", "float", .5, 0.0, 1.0, emptyList()),
        ))

    private fun ready(selected: String = "exposure") = EditorState.Ready(
        Uri.parse("content://tab-navigation-fixture/photo.raw"), 81L,
        Bitmap.createBitmap(32, 24, Bitmap.Config.ARGB_8888),
        listOf(module("exposure", "Exposure"), module("exposure@7", "Exposure 7", false),
            module("sharpen", "Sharpen"), module("retouch@3", "Retouch 3", false),
            module("crop", "Crop")),
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
                {}, {}, {}, {}, {}, {}, onSelect, onEnabled) { module, _ ->
                Text("Parameters for ${module.operation}")
            }
        }
    }

    private fun tab(label: Int) = compose.onNodeWithText(context.getString(label))

    private fun search(query: String) {
        compose.onNodeWithTag("editor_tool_search_button").performClick()
        compose.onNodeWithTag("editor_tool_search_input").performTextReplacement(query)
    }

    @Test fun delayedSelectionFromThePreviousTabCannotLeaveItsToolActive() {
        var current by mutableStateOf(ready())
        val selections = mutableListOf<String>()
        compose.setContent { Fixture(current, { operation ->
            selections += operation
            current = current.copy(selectedModule = operation)
        }) }
        tab(R.string.workspace_tab_geometry).performClick()
        compose.onNodeWithText("Parameters for crop").assertIsDisplayed()
        compose.runOnIdle { assertEquals("crop", current.selectedModule) }
        tab(R.string.workspace_tab_adjustments).performClick()
        compose.onNodeWithText("Parameters for exposure").assertIsDisplayed()
        compose.runOnIdle {
            assertEquals("exposure", current.selectedModule)
            selections.clear()
            // A late collector publication must be reconciled even when the
            // visible target and busy state have not changed.
            current = current.copy(selectedModule = "crop")
        }
        compose.waitForIdle()
        compose.runOnIdle {
            assertEquals("exposure", current.selectedModule)
            assertEquals(listOf("exposure"), selections)
        }
        tab(R.string.workspace_tab_adjustments).assertIsSelected()
        compose.onNodeWithText("Parameters for exposure").assertIsDisplayed()
    }

    @Test fun changingTabsClosesSearchAndShowsThatTabsSelectedTool() {
        var current by mutableStateOf(ready())
        compose.setContent { Fixture(current, { current = current.copy(selectedModule = it) }) }
        search("exposure")
        compose.onNodeWithTag("editor_tool_search_results").assertIsDisplayed()
        tab(R.string.workspace_tab_geometry).performClick()

        tab(R.string.workspace_tab_geometry).assertIsSelected()
        compose.onNodeWithTag("editor_tool_search_input").assertDoesNotExist()
        compose.onNodeWithTag("editor_tool_search_results").assertDoesNotExist()
        compose.onNodeWithText("Parameters for crop").assertIsDisplayed()
        compose.runOnIdle { assertEquals("crop", current.selectedModule) }

        tab(R.string.workspace_tab_adjustments).performClick()
        compose.onNodeWithTag("editor_tool_search_results").assertDoesNotExist()
        compose.onNodeWithText("Parameters for exposure").assertIsDisplayed()
    }

    @Test fun eachTabRestoresItsExactDisabledInstanceWithoutEnablingAnything() {
        var current by mutableStateOf(ready())
        val enabled = mutableListOf<Pair<String, Boolean>>()
        compose.setContent { Fixture(current, { current = current.copy(selectedModule = it) },
            { operation, value -> enabled += operation to value }) }
        compose.onNodeWithTag("editor_tool_picker").performClick()
        compose.onNodeWithText("Exposure 7").performClick()
        compose.onNodeWithText("Parameters for exposure@7").assertIsDisplayed()
        tab(R.string.workspace_tab_detail).performClick()
        compose.onNodeWithTag("editor_tool_picker").performClick()
        compose.onNodeWithText("Retouch 3").performClick()
        compose.onNodeWithText("Parameters for retouch@3").assertIsDisplayed()

        tab(R.string.workspace_tab_adjustments).performClick()
        compose.onNodeWithText("Parameters for exposure@7").assertIsDisplayed()
        compose.runOnIdle { assertEquals("exposure@7", current.selectedModule) }
        tab(R.string.workspace_tab_detail).performClick()
        compose.onNodeWithText("Parameters for retouch@3").assertIsDisplayed()
        compose.runOnIdle {
            assertEquals("retouch@3", current.selectedModule)
            assertFalse(current.modules.single { it.operation == "exposure@7" }.enabled)
            assertFalse(current.modules.single { it.operation == "retouch@3" }.enabled)
            assertTrue(enabled.isEmpty())
        }
    }

    @Test fun busyNavigationDefersGlobalSelectionUntilTheEditFinishes() {
        var current: EditorState by mutableStateOf(ready())
        val selections = mutableListOf<String>()
        compose.setContent { Fixture(current, { operation ->
            selections += operation
            (current as? EditorState.Ready)?.let { current = it.copy(selectedModule = operation) }
        }) }
        compose.runOnIdle { current = EditorState.Working(EditorState.Stage.EDITING, current as EditorState.Ready) }
        tab(R.string.workspace_tab_geometry).performClick()
        compose.onNodeWithText("Parameters for crop").assertIsDisplayed()
        compose.runOnIdle {
            assertTrue(selections.isEmpty())
            assertEquals("exposure", (current as EditorState.Working).previous?.selectedModule)
            current = (current as EditorState.Working).previous!!
        }
        compose.waitForIdle()
        compose.runOnIdle {
            assertEquals("crop", (current as EditorState.Ready).selectedModule)
            assertEquals(listOf("crop"), selections)
        }
    }

    @Test fun settingsRoundTripDoesNotRestoreAnOldSearchOverTheSelectedTool() {
        var current by mutableStateOf(ready("exposure@7"))
        compose.setContent { Fixture(current, { current = current.copy(selectedModule = it) }) }
        search("crop")
        compose.onNodeWithTag("editor_tool_search_results").assertIsDisplayed()
        tab(R.string.workspace_tab_settings).performClick()
        tab(R.string.workspace_tab_adjustments).performClick()

        compose.onNodeWithTag("editor_tool_search_input").assertDoesNotExist()
        compose.onNodeWithTag("editor_tool_search_results").assertDoesNotExist()
        compose.onNodeWithText("Parameters for exposure@7").assertIsDisplayed()
        compose.runOnIdle { assertEquals("exposure@7", current.selectedModule) }
    }
}
