/* SPDX-License-Identifier: GPL-3.0-or-later */
package org.example.darktableandroid

import android.content.pm.ActivityInfo
import android.graphics.Bitmap
import android.net.Uri
import androidx.activity.ComponentActivity
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import androidx.compose.ui.semantics.SemanticsActions
import androidx.compose.ui.semantics.SemanticsProperties
import androidx.compose.ui.test.assertIsDisplayed
import androidx.compose.ui.test.assertIsSelected
import androidx.compose.ui.test.assertTextContains
import androidx.compose.ui.test.assertIsNotEnabled
import androidx.compose.ui.test.assertIsEnabled
import androidx.compose.ui.test.click
import androidx.compose.ui.test.hasSetTextAction
import androidx.compose.ui.test.hasText
import androidx.compose.ui.test.junit4.createComposeRule
import androidx.compose.ui.test.onNodeWithTag
import androidx.compose.ui.test.onNodeWithContentDescription
import androidx.compose.ui.test.onNodeWithText
import androidx.compose.ui.test.performClick
import androidx.compose.ui.test.performScrollTo
import androidx.compose.ui.test.performTextReplacement
import androidx.compose.ui.test.performTouchInput
import org.example.darktableandroid.nativecore.CropRect
import org.example.darktableandroid.nativecore.EditorModule
import org.example.darktableandroid.nativecore.LensCorrection
import org.example.darktableandroid.nativecore.ModuleCrop
import org.example.darktableandroid.nativecore.ModuleParameter
import org.example.darktableandroid.nativecore.ParameterChoice
import org.example.darktableandroid.nativecore.WhiteBalance
import org.example.darktableandroid.nativecore.WhiteBalanceChannel
import org.junit.Assert.assertEquals
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

/** Actual tool widgets, rather than a substitute text card, across every ordered editing-tab pair. */
@RunWith(RobolectricTestRunner::class)
@Config(sdk = [35], qualifiers = "w400dp-h800dp-mdpi")
@GraphicsMode(GraphicsMode.Mode.NATIVE)
class WorkspaceTabPairsTest {
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

    private fun scalar(operation: String, name: String, label: String, enabled: Boolean = true) =
        EditorModule(operation, name, enabled, true,
            listOf(ModuleParameter("amount", label, "float", .5, 0.0, 1.0, emptyList())))

    private fun ready(modules: List<EditorModule>, selected: String = modules.first().operation) = EditorState.Ready(
        Uri.parse("content://tab-pair-fixture/photo.raw"), 101L,
        Bitmap.createBitmap(32, 24, Bitmap.Config.ARGB_8888), modules,
        emptyList(), selectedModule = selected,
    )

    @Composable
    private fun Fixture(
        state: EditorState.Ready,
        onSelect: (String) -> Unit,
        onEnabled: (String, Boolean) -> Unit = { _, _ -> },
        onParameter: (String, String, Double) -> Unit = { _, _, _ -> },
        onCrop: (String, CropRect) -> Unit = { _, _ -> },
    ) {
        DarktableTheme {
            EditorWorkspace(state, RuntimeAvailability(checking = false, available = true),
                {}, {}, {}, {}, {}, {}, onSelect, onEnabled) { module, query ->
                ModuleControls(module = module, editable = module.enabled, selected = true,
                    onSelected = { operation -> operation?.let(onSelect) }, onNewInstance = {},
                    onEnabled = onEnabled, onParameter = onParameter,
                    onCurve = { _, _, _, _ -> }, onCrop = onCrop,
                    onRgbCurveOptions = { _, _, _ -> }, onWhiteBalanceGains = { _, _ -> },
                    onWhiteBalanceSetting = { _, _ -> }, onWhiteBalanceTemperature = { _, _, _ -> },
                    controlQuery = query, sessionId = state.sessionId, showHeader = false)
            }
        }
    }

    private fun tab(label: Int) = compose.onNodeWithText(context.getString(label))
    private fun choose(name: String) {
        compose.onNodeWithTag("editor_tool_picker").performClick()
        compose.onNodeWithText(name).performClick()
    }

    @Test fun allSixOrderedTabPairsReplaceRealNumericAndCropControls() {
        val exposure = scalar("exposure", "Exposure", "Exposure fixture EV")
        val sharpen = scalar("sharpen", "Sharpen", "Sharpen fixture radius")
        val crop = EditorModule("crop", "Crop", true, true, emptyList(),
            crop = ModuleCrop(CropRect(.1f, .1f, .9f, .9f), .01f, true))
        var current by mutableStateOf(ready(listOf(exposure, sharpen, crop)))
        val enabled = mutableListOf<Pair<String, Boolean>>()
        compose.setContent { Fixture(current, { current = current.copy(selectedModule = it) },
            { operation, value -> enabled += operation to value }) }
        val markers = mapOf("exposure" to "Exposure fixture EV", "sharpen" to "Sharpen fixture radius",
            "crop" to context.getString(R.string.crop_bounds_title))
        // This directed circuit covers A→G, G→A, A→D, D→G, G→D, D→A.
        val route = listOf(R.string.workspace_tab_geometry to crop,
            R.string.workspace_tab_adjustments to exposure, R.string.workspace_tab_detail to sharpen,
            R.string.workspace_tab_geometry to crop, R.string.workspace_tab_detail to sharpen,
            R.string.workspace_tab_adjustments to exposure)
        route.forEach { (label, target) ->
            tab(label).performTouchInput { click() }.assertIsSelected()
            compose.onNodeWithTag("editor_tool_picker").assertTextContains(target.name)
            compose.onNodeWithText(markers.getValue(target.operation)).assertIsDisplayed()
            markers.filterKeys { it != target.operation }.values.forEach { previous ->
                compose.onNodeWithText(previous).assertDoesNotExist()
            }
            compose.runOnIdle { assertEquals(target.operation, current.selectedModule) }
        }
        compose.runOnIdle { assertTrue("Tab navigation must not enable any tool", enabled.isEmpty()) }
    }

    @Test fun warmedNonDefaultInstancesKeepTheirOwnFieldsAndEditCallbacksAcrossEveryPair() {
        val exposure = scalar("exposure", "Exposure", "Base exposure fixture")
        val exposureInstance = scalar("exposure@7", "Exposure 7", "Instance exposure fixture")
        val sharpen = scalar("sharpen", "Sharpen", "Base sharpness fixture")
        val sharpenInstance = scalar("sharpen@4", "Sharpen 4", "Instance sharpness fixture")
        val lens = scalar("lens", "Lens correction", "Base lens fixture")
        val lensInstance = scalar("lens@3", "Lens correction 3", "Instance lens fixture")
        var current by mutableStateOf(ready(listOf(exposure, exposureInstance, sharpen, sharpenInstance, lens, lensInstance)))
        val edits = mutableListOf<Triple<String, String, Double>>()
        compose.setContent { Fixture(current, { current = current.copy(selectedModule = it) },
            onParameter = { operation, name, value -> edits += Triple(operation, name, value) }) }
        choose(exposureInstance.name)
        tab(R.string.workspace_tab_detail).performTouchInput { click() }
        choose(sharpenInstance.name)
        tab(R.string.workspace_tab_geometry).performTouchInput { click() }
        choose(lensInstance.name)
        val route = listOf(R.string.workspace_tab_adjustments to exposureInstance,
            R.string.workspace_tab_geometry to lensInstance,
            R.string.workspace_tab_detail to sharpenInstance,
            R.string.workspace_tab_adjustments to exposureInstance,
            R.string.workspace_tab_detail to sharpenInstance,
            R.string.workspace_tab_geometry to lensInstance,
            R.string.workspace_tab_adjustments to exposureInstance)
        route.forEachIndexed { index, (label, target) ->
            tab(label).performTouchInput { click() }.assertIsSelected()
            compose.onNodeWithTag("editor_tool_picker").assertTextContains(target.name)
            val marker = target.parameters.single().label
            compose.onNode(hasSetTextAction() and hasText(marker)).performScrollTo()
                .performTextReplacement((.11 + index * .1).toString())
            compose.onNodeWithText(context.getString(R.string.apply)).performClick()
            compose.runOnIdle {
                assertEquals(target.operation, current.selectedModule)
                assertEquals(target.operation, edits.last().first)
                assertEquals("amount", edits.last().second)
                assertEquals(.11 + index * .1, edits.last().third, .000001)
            }
        }
        compose.runOnIdle { assertEquals(route.size, edits.size) }
    }

    @Test fun whiteBalanceAndNativeLensEditorsReplaceEachOtherInBothDirectionsAfterDetailVisits() {
        val balance = WhiteBalance(listOf(WhiteBalanceChannel(0, "Camera red fixture", 1f),
            WhiteBalanceChannel(1, "Camera green fixture", 1f), WhiteBalanceChannel(2, "Camera blue fixture", 1f)),
            .1f, 8f, 2, "Camera WB fixture", emptyList())
        val whiteBalance = EditorModule("temperature", "White balance", true, true, emptyList(), whiteBalance = balance)
        val correction = LensCorrection("ready", "Lens calibration fixture", 2, false, true, false, false,
            0, "", "", emptyList(), emptyList())
        val lens = EditorModule("lens", "Lens correction", true, true,
            listOf(ModuleParameter("v_strength", "Native lens vignette fixture", "float", .5, 0.0, 1.0, emptyList())),
            lensCorrection = correction)
        val sharpen = scalar("sharpen", "Sharpen", "Sharpness structured fixture")
        var current by mutableStateOf(ready(listOf(whiteBalance, lens, sharpen)))
        compose.setContent { Fixture(current, { current = current.copy(selectedModule = it) }) }
        val wbMarker = context.getString(R.string.white_balance_current, "Camera WB fixture")
        compose.onNodeWithText(wbMarker).assertIsDisplayed()
        listOf(R.string.workspace_tab_geometry to lens, R.string.workspace_tab_adjustments to whiteBalance,
            R.string.workspace_tab_detail to sharpen, R.string.workspace_tab_geometry to lens,
            R.string.workspace_tab_adjustments to whiteBalance, R.string.workspace_tab_geometry to lens,
            R.string.workspace_tab_detail to sharpen, R.string.workspace_tab_adjustments to whiteBalance).forEach { (label, target) ->
            tab(label).performTouchInput { click() }.assertIsSelected()
            compose.onNodeWithTag("editor_tool_picker").assertTextContains(target.name)
            when(target.operation) {
                "temperature" -> {
                    compose.onNodeWithText(wbMarker).assertIsDisplayed()
                    compose.onNodeWithText("Lens calibration fixture").assertDoesNotExist()
                    compose.onNodeWithText("Sharpness structured fixture").assertDoesNotExist()
                }
                "lens" -> {
                    compose.onNodeWithText("Lens calibration fixture").assertIsDisplayed()
                    compose.onNodeWithText(wbMarker).assertDoesNotExist()
                    compose.onNodeWithText("Sharpness structured fixture").assertDoesNotExist()
                }
                else -> {
                    compose.onNodeWithText("Sharpness structured fixture").assertIsDisplayed()
                    compose.onNodeWithText(wbMarker).assertDoesNotExist()
                    compose.onNodeWithText("Lens calibration fixture").assertDoesNotExist()
                }
            }
            compose.runOnIdle { assertEquals(target.operation, current.selectedModule) }
        }
    }

    @Test fun exposureAndMandatoryOrientationSwitchOnPhysicalTapsBeforeAndAfterDetail() {
        val exposure = EditorModule("exposure", "exposure", true, true, listOf(
            ModuleParameter("mode", "Exposure mode fixture", "enum", 0.0, 0.0, 1.0,
                listOf(ParameterChoice(0.0, "manual fixture"), ParameterChoice(1.0, "automatic fixture"))),
            ModuleParameter("exposure", "Exposure EV fixture", "float", 0.0, -18.0, 18.0, emptyList()),
            ModuleParameter("black", "Exposure black fixture", "float", 0.0, -1.0, 1.0, emptyList()),
        ))
        // The processing values and labels are from dt_image_orientation_t in
        // common/image.h; flip is mandatory and has a single native instance.
        val orientation = EditorModule("flip", "orientation", true, false, listOf(
            ModuleParameter("orientation", "Orientation fixture", "enum", 0.0, 0.0, 7.0, listOf(
                ParameterChoice(0.0, "no rotation"), ParameterChoice(1.0, "flip vertically"),
                ParameterChoice(2.0, "flip horizontally"), ParameterChoice(3.0, "rotate 180°"),
                ParameterChoice(4.0, "transpose"), ParameterChoice(5.0, "rotate -90°"),
                ParameterChoice(6.0, "rotate 90°"), ParameterChoice(7.0, "transverse"),
            )),
        ))
        val crop = EditorModule("crop", "crop", true, true, emptyList(),
            crop = ModuleCrop(CropRect(.1f, .1f, .9f, .9f), .01f, true))
        val sharpen = scalar("sharpen", "sharpen", "Orientation route sharpen fixture")
        var current by mutableStateOf(ready(listOf(exposure, crop, orientation, sharpen)))
        val edits = mutableListOf<Triple<String, String, Double>>()
        val enabled = mutableListOf<Pair<String, Boolean>>()
        compose.setContent { Fixture(current, { current = current.copy(selectedModule = it) },
            { operation, value -> enabled += operation to value },
            onParameter = { operation, name, value ->
                edits += Triple(operation, name, value)
                current = current.copy(modules = current.modules.map { module ->
                    if(module.operation == operation) module.copy(parameters = module.parameters.map {
                        if(it.name == name) it.copy(value = value) else it
                    }) else module
                })
            }) }
        // Warm a non-default Geometry selection: crop would otherwise win.
        tab(R.string.workspace_tab_geometry).performTouchInput { click() }
        choose(orientation.name)
        compose.onNodeWithTag("editor_tool_switch").assertIsNotEnabled()
        repeat(2) { pass ->
            repeat(3) { index ->
                tab(R.string.workspace_tab_adjustments).performTouchInput { click() }.assertIsSelected()
                compose.onNodeWithTag("editor_tool_picker").assertTextContains("exposure")
                compose.onNodeWithText("Exposure mode fixture").assertIsDisplayed()
                compose.onNodeWithText("Orientation fixture").assertDoesNotExist()
                val value = .1 + .1 * (pass * 3 + index)
                compose.onNode(hasSetTextAction() and hasText("Exposure EV fixture"))
                    .performScrollTo().performTextReplacement(value.toString())
                // Only the EV draft changed; the black-level Apply stays disabled.
                compose.onNode(hasText(context.getString(R.string.apply)) and
                    androidx.compose.ui.test.isEnabled()).performClick()
                compose.runOnIdle {
                    assertEquals("exposure", current.selectedModule)
                    assertEquals("exposure", edits.last().first)
                    assertEquals("exposure", edits.last().second)
                    assertEquals(value, edits.last().third, .000001)
                }
                tab(R.string.workspace_tab_geometry).performTouchInput { click() }.assertIsSelected()
                compose.onNodeWithTag("editor_tool_picker").assertTextContains("orientation")
                compose.onNodeWithText("Orientation fixture").assertIsDisplayed()
                compose.onNodeWithText("Exposure mode fixture").assertDoesNotExist()
                compose.onNodeWithTag("editor_tool_switch").assertIsNotEnabled()
                compose.onNodeWithContentDescription("Orientation fixture").performClick()
                val expected = if((pass * 3 + index) % 2 == 0) 5.0 else 6.0
                compose.onNodeWithText(if(expected == 5.0) "rotate -90°" else "rotate 90°").performClick()
                compose.runOnIdle {
                    assertEquals("flip", current.selectedModule)
                    assertEquals(Triple("flip", "orientation", expected), edits.last())
                }
            }
            if(pass == 0) {
                tab(R.string.workspace_tab_detail).performTouchInput { click() }.assertIsSelected()
                compose.onNodeWithText("Orientation route sharpen fixture").assertIsDisplayed()
            }
        }
        compose.runOnIdle {
            assertEquals(12, edits.size)
            assertTrue("Orientation/tab selection must never try to toggle a mandatory tool", enabled.isEmpty())
        }
    }

    @Test fun tabNavigationSelectsItsRememberedToolWithinTheClickEvent() {
        val exposure = scalar("exposure", "exposure", "Synchronous exposure fixture")
        val crop = EditorModule("crop", "crop", true, true, emptyList(),
            crop = ModuleCrop(CropRect(.1f, .1f, .9f, .9f), .01f, true))
        val orientation = EditorModule("flip", "orientation", true, false, listOf(
            ModuleParameter("orientation", "Synchronous orientation fixture", "enum", 0.0, 0.0, 7.0,
                listOf(ParameterChoice(0.0, "no rotation"), ParameterChoice(5.0, "rotate -90°"),
                    ParameterChoice(6.0, "rotate 90°"))),
        ))
        var current by mutableStateOf(ready(listOf(exposure, crop, orientation)))
        val selections = mutableListOf<String>()
        compose.setContent { Fixture(current, { operation ->
            selections += operation
            current = current.copy(selectedModule = operation)
        }) }
        tab(R.string.workspace_tab_geometry).performTouchInput { click() }
        choose(orientation.name)
        compose.runOnIdle { assertEquals("flip", current.selectedModule) }

        val selectLight = requireNotNull(tab(R.string.workspace_tab_adjustments)
            .fetchSemanticsNode().config[SemanticsActions.OnClick].action)
        compose.runOnIdle {
            selections.clear()
            assertTrue(selectLight())
            // Do not yield to recomposition or LaunchedEffect here: the tab
            // event itself must select the tool, not just update its label.
            assertEquals("exposure", current.selectedModule)
            assertEquals(listOf("exposure"), selections)
        }

        val selectGeometry = requireNotNull(tab(R.string.workspace_tab_geometry)
            .fetchSemanticsNode().config[SemanticsActions.OnClick].action)
        compose.runOnIdle {
            selections.clear()
            assertTrue(selectGeometry())
            assertEquals("flip", current.selectedModule)
            assertEquals(listOf("flip"), selections)
        }
        compose.onNodeWithTag("editor_tool_picker").assertTextContains("orientation")
        compose.onNodeWithText("Synchronous orientation fixture").assertIsDisplayed()
        compose.onNodeWithText("Synchronous exposure fixture").assertDoesNotExist()
    }

    @Test fun keyedCropControlsRestoreDraftAndScrollButRefreshOffscreenNativeBounds() {
        val initialCrop = ModuleCrop(CropRect(.1f, .1f, .9f, .9f), .01f, true)
        val crop = EditorModule("crop", "crop", true, true, emptyList(), crop = initialCrop)
        var current by mutableStateOf(ready(listOf(
            scalar("exposure", "exposure", "Crop route exposure fixture"),
            scalar("sharpen", "sharpen", "Crop route sharpness fixture"), crop,
        )))
        var nativeEdits = 0
        val enabled = mutableListOf<Pair<String, Boolean>>()
        compose.setContent { Fixture(current, { current = current.copy(selectedModule = it) },
            { operation, value -> enabled += operation to value },
            onParameter = { _, _, _ -> nativeEdits++ }, onCrop = { _, _ -> nativeEdits++ }) }
        fun leftField() = compose.onNode(hasSetTextAction() and hasText(context.getString(R.string.crop_left)))
        fun cropApply() = compose.onNodeWithText(context.getString(R.string.crop_apply))
        fun scrollValue() = compose.onNodeWithTag("editor_parameter_scroll").fetchSemanticsNode()
            .config[SemanticsProperties.VerticalScrollAxisRange].value()

        tab(R.string.workspace_tab_geometry).performTouchInput { click() }
        leftField().performScrollTo().performTextReplacement("0.2")
        cropApply().performScrollTo().assertIsDisplayed().assertIsEnabled()
        val rememberedScroll = scrollValue()
        assertTrue("The crop fixture must scroll before leaving it", rememberedScroll > 0f)

        tab(R.string.workspace_tab_adjustments).performTouchInput { click() }
        tab(R.string.workspace_tab_detail).performTouchInput { click() }
        tab(R.string.workspace_tab_geometry).performTouchInput { click() }
        assertEquals("Returning to the same native crop restores its scroll", rememberedScroll, scrollValue(), 1f)
        cropApply().assertIsDisplayed().assertIsEnabled()
        leftField().assertTextContains("0.2")
        compose.runOnIdle {
            assertEquals("crop", current.selectedModule)
            assertEquals(.1f, current.modules.single { it.operation == "crop" }.crop!!.bounds.left)
        }

        tab(R.string.workspace_tab_adjustments).performTouchInput { click() }
        compose.runOnIdle {
            // Simulate Undo publishing a new native snapshot while crop is
            // offscreen. Restore position, but never revive the old .2 draft.
            current = current.copy(modules = current.modules.map { module ->
                if(module.operation == "crop") module.copy(crop = initialCrop.copy(
                    bounds = initialCrop.bounds.copy(left = .3f))) else module
            })
        }
        tab(R.string.workspace_tab_geometry).performTouchInput { click() }
        assertEquals("Native refresh must preserve the tool's scroll", rememberedScroll, scrollValue(), 1f)
        cropApply().assertIsDisplayed().assertIsNotEnabled()
        leftField().assertTextContains("0.3")
        compose.runOnIdle {
            assertEquals("crop", current.selectedModule)
            assertEquals(0, nativeEdits)
            assertTrue("Draft restoration and navigation must not toggle a tool", enabled.isEmpty())
        }
    }
}
