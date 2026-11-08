/* SPDX-License-Identifier: GPL-3.0-or-later */
package org.example.darktableandroid

import android.content.pm.ActivityInfo
import android.graphics.Bitmap
import android.graphics.BitmapFactory
import android.graphics.Canvas
import android.graphics.Color
import android.net.Uri
import android.view.View
import androidx.activity.ComponentActivity
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Typography
import androidx.compose.runtime.Composable
import androidx.compose.runtime.SideEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import androidx.compose.ui.geometry.Rect
import androidx.compose.ui.semantics.SemanticsActions
import androidx.compose.ui.semantics.SemanticsProperties
import androidx.compose.ui.semantics.getOrNull
import androidx.compose.ui.test.SemanticsNodeInteraction
import androidx.compose.ui.test.assertIsDisplayed
import androidx.compose.ui.test.assertIsNotEnabled
import androidx.compose.ui.test.assertIsSelected
import androidx.compose.ui.test.assertTextContains
import androidx.compose.ui.test.hasSetTextAction
import androidx.compose.ui.test.hasText
import androidx.compose.ui.test.junit4.createComposeRule
import androidx.compose.ui.test.onNodeWithContentDescription
import androidx.compose.ui.test.onNodeWithTag
import androidx.compose.ui.test.onNodeWithText
import androidx.compose.ui.test.onRoot
import androidx.compose.ui.test.performClick
import androidx.compose.ui.test.performScrollTo
import androidx.compose.ui.test.performTextReplacement
import androidx.test.runner.lifecycle.ActivityLifecycleMonitorRegistry
import androidx.test.runner.lifecycle.Stage
import java.io.File
import kotlin.math.roundToInt
import org.example.darktableandroid.nativecore.EditHistory
import org.example.darktableandroid.nativecore.EditorModule
import org.example.darktableandroid.nativecore.ExportFormat
import org.example.darktableandroid.nativecore.ModuleParameter
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Rule
import org.junit.Test
import org.junit.rules.ExternalResource
import org.junit.rules.RuleChain
import org.junit.rules.TestRule
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.RuntimeEnvironment
import org.robolectric.Shadows.shadowOf
import org.robolectric.annotation.Config
import org.robolectric.annotation.GraphicsMode

/** Production Compose controls and geometry; the bundled JPEG is only a UI fixture, not a native RAW render. */
@RunWith(RobolectricTestRunner::class)
@Config(sdk = [35], qualifiers = "w400dp-h800dp-mdpi")
@GraphicsMode(GraphicsMode.Mode.NATIVE)
class DarktableThemeTest {
    private val compose = createComposeRule()
    private val context: android.content.Context get() = RuntimeEnvironment.getApplication()

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
            ModuleParameter("exposure", "Exposure", "float", 0.0, -4.0, 4.0, emptyList()),
            ModuleParameter("automatic", "Automatic", "bool", 0.0, 0.0, 1.0, emptyList()),
        ))

    private fun preview(): Bitmap {
        val bundled = File("../../data/style/image-1.jpg")
        if(bundled.isFile) BitmapFactory.decodeFile(bundled.absolutePath)?.let { return it }
        return Bitmap.createBitmap(320, 180, Bitmap.Config.ARGB_8888).apply {
            for(y in 0 until height) for(x in 0 until width) {
                setPixel(x, y, Color.rgb(40 + 150 * x / width, 45 + 120 * y / height, 160 - 70 * x / width))
            }
        }
    }

    private fun ready() = EditorState.Ready(
        Uri.parse("content://theme-fixture/photo.raw"), 91L, preview(),
        listOf(module("exposure", "Exposure"), module("exposure@7", "Exposure 7", false),
            module("sharpen", "Sharpen", false), module("crop", "Crop")),
        listOf(ExportFormat("png", "PNG", "png", "image/png", 8)),
        history = EditHistory(true, true, 1, 2), selectedModule = "exposure",
    )

    @Composable
    private fun Fixture(
        state: EditorState.Ready,
        styled: Boolean = true,
        onSelect: (String) -> Unit = {},
        onEnabled: (String, Boolean) -> Unit = { _, _ -> },
        onParameter: (String, String, Double) -> Unit = { _, _, _ -> },
        onAction: (String) -> Unit = {},
        onTypography: (Typography) -> Unit = {},
    ) {
        val content: @Composable () -> Unit = {
            val typography = MaterialTheme.typography
            SideEffect { onTypography(typography) }
            EditorWorkspace(state, RuntimeAvailability(checking = false, available = true),
                { onAction("open") }, { onAction("export") }, { onAction("undo") }, { onAction("redo") },
                {}, {}, onSelect, onEnabled) { module, query ->
                ModuleControls(module = module, editable = module.enabled, selected = true,
                    onSelected = { operation -> operation?.let(onSelect) },
                    onNewInstance = {}, onEnabled = onEnabled, onParameter = onParameter,
                    onCurve = { _, _, _, _ -> }, onCrop = { _, _ -> },
                    onRgbCurveOptions = { _, _, _ -> }, onWhiteBalanceGains = { _, _ -> },
                    onWhiteBalanceSetting = { _, _ -> }, onWhiteBalanceTemperature = { _, _, _ -> },
                    controlQuery = query, sessionId = state.sessionId, showHeader = false)
            }
        }
        if(styled) DarktableTheme(content = content) else MaterialTheme(content = content)
    }

    private data class NodeState(
        val bounds: Rect,
        val clickable: Boolean,
        val editable: Boolean,
        val progress: Boolean,
        val disabled: Boolean,
        val selected: Boolean?,
        val toggle: String?,
    )

    private fun SemanticsNodeInteraction.state(): NodeState = fetchSemanticsNode().let { node ->
        NodeState(node.boundsInRoot, node.config.contains(SemanticsActions.OnClick),
            node.config.contains(SemanticsActions.SetText), node.config.contains(SemanticsActions.SetProgress),
            node.config.contains(SemanticsProperties.Disabled), node.config.getOrNull(SemanticsProperties.Selected),
            node.config.getOrNull(SemanticsProperties.ToggleableState)?.toString())
    }

    private fun workspaceGeometry(): Map<String, NodeState> = buildMap {
        listOf("editor_preview_pane", "editor_tools_pane", "editor_tool_picker", "editor_tool_switch",
            "editor_tool_search_button", "editor_open", "editor_undo", "editor_redo", "editor_export",
            "editor_parameter_scroll").forEach { tag ->
            put(tag, compose.onNodeWithTag(tag).state())
        }
        listOf(R.string.workspace_tab_adjustments, R.string.workspace_tab_detail,
            R.string.workspace_tab_geometry, R.string.workspace_tab_settings).forEach { label ->
            put("tab-$label", compose.onNodeWithText(context.getString(label)).state())
        }
        put("parameter-slider", compose.onNodeWithContentDescription(
            context.getString(R.string.parameter_slider_description, "Exposure")).state())
        put("parameter-input", compose.onNode(hasSetTextAction() and hasText("Exposure")).state())
        put("parameter-switch", compose.onNodeWithContentDescription("Automatic").state())
        put("parameter-section", compose.onNodeWithText(context.getString(R.string.module_adjustments)).state())
        put("parameter-apply", compose.onNodeWithText(context.getString(R.string.apply)).state())
    }

    private fun capture(name: String): Bitmap {
        val bounds = compose.onRoot().fetchSemanticsNode().boundsInRoot
        // PixelCopy waits for a hardware frame callback that the host runtime
        // does not deliver. Native graphics can draw the actual measured view
        // tree into a software canvas, using the same Compose drawing code.
        val bitmap = compose.runOnIdle {
            val activity = ActivityLifecycleMonitorRegistry.getInstance()
                .getActivitiesInStage(Stage.RESUMED).single()
            val content = activity.findViewById<View>(android.R.id.content)
            val full = Bitmap.createBitmap(content.width, content.height, Bitmap.Config.ARGB_8888)
            content.draw(Canvas(full))
            val left = bounds.left.roundToInt()
            val top = bounds.top.roundToInt()
            val width = bounds.width.roundToInt()
            val height = bounds.height.roundToInt()
            check(left >= 0 && top >= 0 && width > 0 && height > 0
                && left + width <= full.width && top + height <= full.height)
            Bitmap.createBitmap(full, left, top, width, height)
        }
        val directory = File(System.getProperty("darktable.theme.capture.dir") ?: "build/reports/theme-preview")
        check(directory.isDirectory || directory.mkdirs())
        File(directory, name).outputStream().use { output ->
            check(bitmap.compress(Bitmap.CompressFormat.PNG, 100, output))
        }
        return bitmap
    }

    @Test fun desktopStylingPreservesAllWorkspaceBoundsActionsAndFontMetrics() {
        val current = ready()
        var styled by mutableStateOf(false)
        var typography: Typography? = null
        val actions = mutableListOf<String>()
        val selections = mutableListOf<String>()
        val edits = mutableListOf<Pair<String, Boolean>>()
        compose.setContent { Fixture(current, styled, onSelect = { selections += it },
            onEnabled = { operation, enabled -> edits += operation to enabled },
            onAction = { actions += it }, onTypography = { typography = it }) }
        val before = workspaceGeometry()
        val beforeTypography = typography
        val beforeImage = capture("workspace-before.png")
        listOf("open", "undo", "redo", "export").forEach { action ->
            compose.onNodeWithTag("editor_$action").performClick()
        }

        compose.runOnIdle { styled = true }
        compose.waitForIdle()
        val after = workspaceGeometry()
        val afterImage = capture("workspace-desktop-grey.png")
        assertEquals("The theme must preserve every control's bounds and semantic actions", before, after)
        assertEquals("Font sizes, weights and line metrics must stay unchanged", beforeTypography, typography)
        assertEquals(beforeImage.width, afterImage.width)
        assertEquals(beforeImage.height, afterImage.height)
        val root = compose.onRoot().fetchSemanticsNode().boundsInRoot
        val preview = after.getValue("editor_preview_pane").bounds
        val x = (preview.left - root.left + 2).toInt()
        val y = (preview.center.y - root.top).toInt()
        assertEquals("The desktop photo surround is neutral middle grey", Color.rgb(119, 119, 119), afterImage.getPixel(x, y))
        assertFalse("The screenshot must render the new palette", beforeImage.sameAs(afterImage))
        listOf("open", "undo", "redo", "export").forEach { action ->
            compose.onNodeWithTag("editor_$action").performClick()
        }
        compose.runOnIdle {
            assertEquals(listOf("open", "undo", "redo", "export", "open", "undo", "redo", "export"), actions)
            assertTrue("A visual restyle must not select or enable another tool", selections.isEmpty() && edits.isEmpty())
        }
    }

    @Test fun styledSearchStillOpensTheExactDisabledToolAndRemembersItThroughSettings() {
        var current by mutableStateOf(ready())
        val enabled = mutableListOf<Pair<String, Boolean>>()
        compose.setContent { Fixture(current, onSelect = { current = current.copy(selectedModule = it) },
            onEnabled = { operation, value -> enabled += operation to value }) }
        compose.onNodeWithTag("editor_tool_search_button").performClick()
        compose.onNodeWithTag("editor_tool_search_input").performTextReplacement("sharpen")
        compose.onNodeWithTag("editor_tool_search_result_sharpen").assertIsDisplayed()
        capture("tool-search-desktop-grey.png")
        compose.onNodeWithTag("editor_tool_search_result_sharpen").performClick()
        compose.onNodeWithText(context.getString(R.string.workspace_tab_detail)).assertIsSelected()
        compose.onNodeWithTag("editor_tool_picker").assertTextContains("Sharpen")
        compose.onNodeWithTag("editor_tool_search_input").assertDoesNotExist()
        compose.onNodeWithContentDescription(context.getString(R.string.parameter_slider_description, "Exposure"))
            .assertIsNotEnabled()
        capture("disabled-tool-desktop-grey.png")
        compose.onNodeWithText(context.getString(R.string.workspace_tab_settings)).performClick()
        compose.onNodeWithText(context.getString(R.string.workspace_tab_detail)).performClick()
        compose.onNodeWithTag("editor_tool_picker").assertTextContains("Sharpen")
        compose.runOnIdle {
            assertEquals("sharpen", current.selectedModule)
            assertFalse(current.modules.single { it.operation == "sharpen" }.enabled)
            assertTrue(enabled.isEmpty())
        }
    }

    @Test fun styledNumericApplyBooleanAndPowerControlsKeepTheirOriginalCallbacks() {
        var current by mutableStateOf(ready())
        val parameters = mutableListOf<Triple<String, String, Double>>()
        val enabled = mutableListOf<Pair<String, Boolean>>()
        compose.setContent { Fixture(current, onSelect = { current = current.copy(selectedModule = it) },
            onEnabled = { operation, value ->
                enabled += operation to value
                current = current.copy(modules = current.modules.map { if(it.operation == operation) it.copy(enabled = value) else it })
            }, onParameter = { operation, name, value ->
                parameters += Triple(operation, name, value)
                current = current.copy(modules = current.modules.map { module ->
                    if(module.operation == operation) module.copy(parameters = module.parameters.map {
                        if(it.name == name) it.copy(value = value) else it
                    }) else module
                })
            }) }
        compose.onNode(hasSetTextAction() and hasText("Exposure")).performTextReplacement("0.5")
        compose.onNodeWithText(context.getString(R.string.apply)).performClick()
        compose.onNodeWithContentDescription("Automatic").performScrollTo().performClick()
        compose.onNodeWithTag("editor_tool_switch").performClick()
        compose.onNodeWithContentDescription("Automatic").assertIsNotEnabled()
        compose.runOnIdle {
            assertEquals(listOf(Triple("exposure", "exposure", .5), Triple("exposure", "automatic", 1.0)), parameters)
            assertEquals(listOf("exposure" to false), enabled)
            assertEquals("exposure", current.selectedModule)
        }
    }
}
