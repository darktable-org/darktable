/* SPDX-License-Identifier: GPL-3.0-or-later */
package org.example.darktableandroid

import android.graphics.Bitmap
import android.net.Uri
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.height
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.semantics.SemanticsProperties
import androidx.compose.ui.test.SemanticsMatcher
import androidx.compose.ui.test.assertCountEquals
import androidx.compose.ui.test.assertIsDisplayed
import androidx.compose.ui.test.assertIsEnabled
import androidx.compose.ui.test.assertIsNotEnabled
import androidx.compose.ui.test.assertIsSelected
import androidx.compose.ui.test.assertTextContains
import androidx.compose.ui.test.hasSetTextAction
import androidx.compose.ui.test.hasText
import androidx.compose.ui.test.junit4.StateRestorationTester
import androidx.compose.ui.test.junit4.createComposeRule
import androidx.compose.ui.test.onNodeWithContentDescription
import androidx.compose.ui.test.onNodeWithTag
import androidx.compose.ui.test.onNodeWithText
import androidx.compose.ui.test.performClick
import androidx.compose.ui.test.performScrollTo
import androidx.compose.ui.test.performTouchInput
import androidx.compose.ui.test.performTextReplacement
import androidx.compose.ui.test.swipeDown
import androidx.compose.ui.unit.dp
import androidx.test.platform.app.InstrumentationRegistry
import org.example.darktableandroid.nativecore.EditHistory
import org.example.darktableandroid.nativecore.CropRect
import org.example.darktableandroid.nativecore.EditorModule
import org.example.darktableandroid.nativecore.ExportFormat
import org.example.darktableandroid.nativecore.ModuleParameter
import org.example.darktableandroid.nativecore.ModuleCrop
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Rule
import org.junit.Test

/** Production workspace layout and callbacks; the bitmap is a component fixture, not a native render. */
class EditorWorkspaceTest {
    @get:Rule val compose = createComposeRule()
    private val context get() = InstrumentationRegistry.getInstrumentation().targetContext

    private fun module(operation: String, name: String, enabled: Boolean = true) =
        EditorModule(operation, name, enabled, true, listOf(
            ModuleParameter("amount", "Test amount", "float", 0.5, 0.0, 1.0, emptyList()),
        ))

    private fun ready(selected: String = "exposure"): EditorState.Ready = EditorState.Ready(
        Uri.parse("content://workspace-fixture/photo.raw"), 27L,
        Bitmap.createBitmap(32, 24, Bitmap.Config.ARGB_8888),
        listOf(module("exposure", "Exposure"), module("exposure@7", "Exposure 7", false),
            module("sharpen", "Sharpen"), module("crop", "Crop")),
        listOf(ExportFormat("png", "PNG", "png", "image/png", 8)),
        history = EditHistory(true, true, 1, 2), selectedModule = selected,
    )

    @Composable
    private fun Fixture(
        state: EditorState,
        runtime: RuntimeAvailability = RuntimeAvailability(checking = false, available = true),
        onSelect: (String) -> Unit = {},
        onEnabled: (String, Boolean) -> Unit = { _, _ -> },
        onCancel: () -> Unit = {},
        onRetry: (Uri) -> Unit = {},
        longControls: Boolean = false,
    ) {
        MaterialTheme {
            EditorWorkspace(state, runtime, {}, {}, {}, {}, onCancel, onRetry, onSelect, onEnabled) { module, _ ->
                Column {
                    Text("Parameters for ${module.operation}")
                    ParameterControl(module.parameters.single(), module.enabled && state !is EditorState.Working) {}
                    if(longControls) repeat(40) { index ->
                        Text("Scrollable setting $index", Modifier.height(40.dp))
                    }
                }
            }
        }
    }

    @Test fun previewAndParametersShareFixedHalvesWithoutAnAppTitle() {
        compose.setContent { Fixture(ready()) }
        compose.onNodeWithContentDescription(context.getString(R.string.preview_description)).assertIsDisplayed()
        compose.onNodeWithText("Parameters for exposure").assertIsDisplayed()
        compose.onNodeWithText(context.getString(R.string.app_name)).assertDoesNotExist()
        val preview = compose.onNodeWithTag("editor_preview_pane").fetchSemanticsNode().boundsInRoot
        val tools = compose.onNodeWithTag("editor_tools_pane").fetchSemanticsNode().boundsInRoot
        assertTrue(preview.height > 0f && tools.height > 0f)
        assertEquals(preview.height, tools.height, 2f)
        assertTrue("Preview must stay above the tool pane", preview.bottom <= tools.top + 1f)
    }

    @Test fun choosingToolsAndTabsRetainsExactInstancesWithoutEnablingThem() {
        var current by mutableStateOf(ready())
        val selections = mutableListOf<String>()
        val enabled = mutableListOf<Pair<String, Boolean>>()
        compose.setContent { Fixture(current, onSelect = { operation ->
            selections += operation
            current = current.copy(selectedModule = operation)
        }, onEnabled = { operation, value -> enabled += operation to value }) }
        compose.onNodeWithTag("editor_tool_picker").performClick()
        compose.onNodeWithText("Exposure 7").performClick()
        compose.onNodeWithText("Parameters for exposure@7").assertIsDisplayed()
        compose.onNodeWithText(context.getString(R.string.workspace_tab_geometry)).performClick()
        compose.onNodeWithText("Parameters for crop").assertIsDisplayed()
        compose.onNodeWithText(context.getString(R.string.workspace_tab_detail)).performClick()
        compose.onNodeWithText("Parameters for sharpen").assertIsDisplayed()
        compose.onNodeWithText(context.getString(R.string.workspace_tab_adjustments)).performClick()
        compose.onNodeWithText("Parameters for exposure@7").assertIsDisplayed()
        compose.runOnIdle {
            assertTrue(selections.contains("exposure@7"))
            assertEquals("exposure@7", current.selectedModule)
            assertTrue("Selecting a tool or category must not change its processing state", enabled.isEmpty())
        }
    }

    @Test fun explicitPowerSwitchActivatesOnlyTheSelectedInstanceOnce() {
        var current by mutableStateOf(ready("exposure@7"))
        val enabled = mutableListOf<Pair<String, Boolean>>()
        compose.setContent { Fixture(current, onSelect = { current = current.copy(selectedModule = it) },
            onEnabled = { operation, value ->
                enabled += operation to value
                current = current.copy(modules = current.modules.map {
                    if(it.operation == operation) it.copy(enabled = value) else it
                })
            }) }
        val slider = context.getString(R.string.parameter_slider_description, "Test amount")
        compose.onNodeWithContentDescription(slider).assertIsNotEnabled()
        compose.onNodeWithTag("editor_tool_switch").performClick()
        compose.onNodeWithContentDescription(slider).assertIsEnabled()
        compose.runOnIdle {
            assertEquals(listOf("exposure@7" to true), enabled)
            assertTrue(current.modules.single { it.operation == "exposure" }.enabled)
        }
    }

    @Test fun verticalParameterScrollKeepsPreviewAndStickyToolControlsOnScreen() {
        compose.setContent { Fixture(ready(), longControls = true) }
        compose.onNodeWithText("Scrollable setting 39").performScrollTo().assertIsDisplayed()
        compose.onNodeWithTag("editor_parameter_scroll").performTouchInput { swipeDown() }
        compose.onNodeWithTag("editor_preview_pane").assertIsDisplayed()
        compose.onNodeWithTag("editor_tool_picker").assertIsDisplayed()
        compose.onNodeWithTag("editor_tool_switch").assertIsDisplayed()
        compose.onNodeWithText(context.getString(R.string.workspace_tab_adjustments)).assertIsSelected()
    }

    @Test fun settingsRoundTripRetainsEachCategoryToolAndItsParameterScroll() {
        var current by mutableStateOf(ready())
        val enabled = mutableListOf<Pair<String, Boolean>>()
        compose.setContent { Fixture(current, onSelect = { current = current.copy(selectedModule = it) },
            onEnabled = { operation, value -> enabled += operation to value }, longControls = true) }
        compose.onNodeWithTag("editor_tool_picker").performClick()
        compose.onNodeWithText("Exposure 7").performClick()
        compose.onNodeWithText("Scrollable setting 39").performScrollTo().assertIsDisplayed()
        val rememberedScroll = compose.onNodeWithTag("editor_parameter_scroll").fetchSemanticsNode()
            .config[SemanticsProperties.VerticalScrollAxisRange].value()
        assertTrue("The fixture must actually scroll before leaving its category", rememberedScroll > 0f)

        compose.onNodeWithText(context.getString(R.string.workspace_tab_geometry)).performClick()
        compose.onNodeWithText("Parameters for crop").assertIsDisplayed()
        compose.onNodeWithText(context.getString(R.string.workspace_tab_settings)).performClick()
        compose.onNodeWithText(context.getString(R.string.workspace_license_title)).performScrollTo().assertIsDisplayed()
        compose.onNodeWithText(context.getString(R.string.workspace_tab_adjustments)).performClick()

        compose.onNodeWithText("Scrollable setting 39").assertIsDisplayed()
        val restoredScroll = compose.onNodeWithTag("editor_parameter_scroll").fetchSemanticsNode()
            .config[SemanticsProperties.VerticalScrollAxisRange].value()
        assertEquals(rememberedScroll, restoredScroll, 1f)
        compose.runOnIdle {
            assertEquals("exposure@7", current.selectedModule)
            assertTrue("Settings and category navigation must not enable a tool", enabled.isEmpty())
        }
    }

    @Test fun savedInstanceRestorationKeepsTheCurrentCategoryAndSettingsPage() {
        var current by mutableStateOf(ready())
        val restoration = StateRestorationTester(compose)
        restoration.setContent { Fixture(current, onSelect = { current = current.copy(selectedModule = it) }) }
        compose.onNodeWithText(context.getString(R.string.workspace_tab_geometry)).performClick()
        compose.onNodeWithText("Parameters for crop").assertIsDisplayed()
        restoration.emulateSavedInstanceStateRestore()
        compose.onNodeWithText(context.getString(R.string.workspace_tab_geometry)).assertIsSelected()
        compose.onNodeWithText("Parameters for crop").assertIsDisplayed()

        compose.onNodeWithText(context.getString(R.string.workspace_tab_settings)).performClick()
        restoration.emulateSavedInstanceStateRestore()
        compose.onNodeWithText(context.getString(R.string.workspace_tab_settings)).assertIsSelected()
        compose.onNodeWithText(context.getString(R.string.workspace_license_title)).performScrollTo().assertIsDisplayed()
    }

    @Test fun returningToCropUsesRestoredNativeBoundsInsteadOfAnOldUnappliedDraft() {
        val crop = ModuleCrop(CropRect(.1f, .1f, .9f, .9f), .01f, true)
        var current by mutableStateOf(ready().let { state ->
            state.copy(modules = state.modules.map {
                if(it.operation == "crop") it.copy(parameters = emptyList(), crop = crop) else it
            })
        })
        var edits = 0
        compose.setContent { MaterialTheme {
            EditorWorkspace(current, RuntimeAvailability(checking = false, available = true),
                {}, {}, {}, {}, {}, {}, { current = current.copy(selectedModule = it) },
                { _, _ -> edits++ }) { module, _ ->
                ModuleControls(module, module.enabled, true, {}, { edits++ }, { _, _ -> edits++ },
                    { _, _, _ -> edits++ }, { _, _, _, _ -> edits++ }, { _, _ -> edits++ },
                    { _, _, _ -> edits++ }, { _, _ -> edits++ }, { _, _ -> edits++ },
                    { _, _, _ -> edits++ }, showHeader = false)
            }
        } }
        compose.onNodeWithText(context.getString(R.string.workspace_tab_geometry)).performClick()
        fun leftField() = compose.onNode(hasSetTextAction() and hasText(context.getString(R.string.crop_left)))
        leftField().performScrollTo().performTextReplacement("0.2")
        leftField().assertTextContains("0.2")
        val rememberedScroll = compose.onNodeWithTag("editor_parameter_scroll").fetchSemanticsNode()
            .config[SemanticsProperties.VerticalScrollAxisRange].value()
        compose.onNodeWithText(context.getString(R.string.workspace_tab_adjustments)).performClick()
        compose.runOnIdle {
            // Simulate Undo publishing a different native snapshot while crop is offscreen.
            current = current.copy(modules = current.modules.map {
                if(it.operation == "crop") it.copy(crop = crop.copy(bounds = crop.bounds.copy(left = .3f))) else it
            })
        }
        compose.onNodeWithText(context.getString(R.string.workspace_tab_geometry)).performClick()
        leftField().assertTextContains("0.3")
        val restoredScroll = compose.onNodeWithTag("editor_parameter_scroll").fetchSemanticsNode()
            .config[SemanticsProperties.VerticalScrollAxisRange].value()
        assertEquals(rememberedScroll, restoredScroll, 1f)
        compose.runOnIdle {
            assertEquals("crop", current.selectedModule)
            assertEquals("Draft entry and history refresh must not submit another edit", 0, edits)
        }
    }

    @Test fun licenseRemainsReachableWithoutAnImageOrProcessingEngine() {
        compose.setContent { Fixture(EditorState.Empty,
            RuntimeAvailability(checking = false, available = false, message = "Engine unavailable")) }
        compose.onNodeWithText(context.getString(R.string.workspace_tab_settings)).performClick()
        compose.onNodeWithText(context.getString(R.string.workspace_license_title)).performScrollTo().assertIsDisplayed()
        compose.onNodeWithText("GPL-3.0-or-later", substring = true).performScrollTo().assertIsDisplayed()
        compose.onNodeWithTag("editor_open").assertIsNotEnabled()
    }

    @Test fun renderingKeepsCancelReachableAndDisablesMutatingActions() {
        var current: EditorState by mutableStateOf(EditorState.Working(EditorState.Stage.EDITING, ready()))
        var cancellations = 0
        val selections = mutableListOf<String>()
        val enabled = mutableListOf<Pair<String, Boolean>>()
        compose.setContent { Fixture(current, onSelect = { operation ->
            selections += operation
            (current as? EditorState.Ready)?.let { current = it.copy(selectedModule = operation) }
        }, onEnabled = { operation, value -> enabled += operation to value }, onCancel = { cancellations++ }) }
        compose.onNodeWithTag("editor_preview_pane").assertIsDisplayed()
        compose.onNodeWithTag("editor_tool_switch").assertIsNotEnabled()
        compose.onNodeWithTag("editor_undo").assertIsNotEnabled()
        compose.onNodeWithTag("editor_redo").assertIsNotEnabled()
        compose.onNodeWithTag("editor_export").assertIsNotEnabled()
        compose.onNodeWithText(context.getString(R.string.cancel)).performClick()
        compose.onNodeWithText(context.getString(R.string.workspace_tab_settings)).performClick()
        compose.onNodeWithText(context.getString(R.string.workspace_license_title)).performScrollTo().assertIsDisplayed()
        compose.onNodeWithText(context.getString(R.string.workspace_tab_geometry)).performClick()
        compose.onNodeWithTag("editor_tool_switch").assertIsNotEnabled()
        compose.runOnIdle {
            assertEquals(1, cancellations)
            assertTrue(selections.isEmpty())
            assertTrue(enabled.isEmpty())
            current = ready()
        }
        compose.onNodeWithText(context.getString(R.string.workspace_tab_geometry)).assertIsSelected()
        compose.onNodeWithText("Parameters for crop").assertIsDisplayed()
        compose.runOnIdle { assertEquals(listOf("crop"), selections) }
    }

    @Test fun embeddedModuleControlsShowParametersWithoutAnotherTitleOrPowerSwitch() {
        val selected = module("exposure@7", "Exposure 7", false)
        var callbacks = 0
        compose.setContent { MaterialTheme {
            ModuleControls(selected, false, true, { callbacks++ }, { callbacks++ }, { _, _ -> callbacks++ },
                { _, _, _ -> callbacks++ }, { _, _, _, _ -> callbacks++ }, { _, _ -> callbacks++ },
                { _, _, _ -> callbacks++ }, { _, _ -> callbacks++ }, { _, _ -> callbacks++ },
                { _, _, _ -> callbacks++ }, showHeader = false)
        } }
        compose.onNodeWithText("Test amount").assertIsDisplayed()
        compose.onNodeWithText(selected.name).assertDoesNotExist()
        compose.onAllNodes(SemanticsMatcher.expectValue(SemanticsProperties.Role, Role.Switch)).assertCountEquals(0)
        compose.onNodeWithContentDescription(context.getString(R.string.parameter_slider_description, "Test amount"))
            .assertIsNotEnabled()
        compose.runOnIdle { assertEquals("Embedding or selecting a disabled tool must not edit it", 0, callbacks) }
    }
}
