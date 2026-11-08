/* SPDX-License-Identifier: GPL-3.0-or-later */
package org.example.darktableandroid

import android.content.Context
import android.content.pm.ActivityInfo
import android.graphics.Bitmap
import android.graphics.BitmapFactory
import android.graphics.Canvas
import android.graphics.drawable.ColorDrawable
import android.net.Uri
import android.view.View
import androidx.activity.ComponentActivity
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import androidx.compose.ui.geometry.Rect
import androidx.compose.ui.test.assertIsDisplayed
import androidx.compose.ui.test.assertTextContains
import androidx.compose.ui.test.hasSetTextAction
import androidx.compose.ui.test.hasText
import androidx.compose.ui.test.junit4.createComposeRule
import androidx.compose.ui.test.onNodeWithTag
import androidx.compose.ui.test.onNodeWithText
import androidx.compose.ui.test.onRoot
import androidx.compose.ui.test.performClick
import androidx.compose.ui.test.performScrollTo
import androidx.compose.ui.test.performTextReplacement
import androidx.core.view.WindowCompat
import androidx.test.runner.lifecycle.ActivityLifecycleMonitorRegistry
import androidx.test.runner.lifecycle.Stage
import androidx.test.filters.SdkSuppress
import java.io.File
import kotlin.math.max
import kotlin.math.min
import kotlin.math.pow
import kotlin.math.roundToInt
import org.example.darktableandroid.nativecore.EditHistory
import org.example.darktableandroid.nativecore.EditorModule
import org.example.darktableandroid.nativecore.ExportFormat
import org.example.darktableandroid.nativecore.ModuleParameter
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertSame
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

/** Host theme rendering and interaction, without native RAW processing or physical system bars. */
@RunWith(RobolectricTestRunner::class)
@Config(sdk = [35], qualifiers = "en-rUS-w400dp-h800dp-mdpi")
@GraphicsMode(GraphicsMode.Mode.NATIVE)
class AppThemeTest {
    private val compose = createComposeRule()
    private val context: Context get() = RuntimeEnvironment.getApplication()

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

    private fun ready() = EditorState.Ready(
        Uri.parse("content://theme-choice-fixture/photo.raw"), 173L,
        File("../../data/style/image-1.jpg").takeIf { it.isFile }?.let { BitmapFactory.decodeFile(it.absolutePath) }
            ?: Bitmap.createBitmap(32, 24, Bitmap.Config.ARGB_8888),
        listOf(EditorModule("exposure", "Exposure", true, true, listOf(
            ModuleParameter("exposure", "Exposure", "float", 0.0, -4.0, 4.0, emptyList()),
        ))),
        listOf(ExportFormat("png", "PNG", "png", "image/png", 8)),
        history = EditHistory(true, true, 1, 2), selectedModule = "exposure",
    )

    @Composable
    private fun Workspace(
        state: EditorState.Ready,
        onSelect: (String) -> Unit = {},
        onEnabled: (String, Boolean) -> Unit = { _, _ -> },
        onParameter: (String, String, Double) -> Unit = { _, _, _ -> },
    ) {
        EditorWorkspace(state, RuntimeAvailability(checking = false, available = true),
            {}, {}, {}, {}, {}, {}, onSelect, onEnabled) { module, query ->
            ModuleControls(module = module, editable = module.enabled, selected = true,
                onSelected = { operation -> operation?.let(onSelect) }, onNewInstance = {},
                onEnabled = onEnabled, onParameter = onParameter,
                onCurve = { _, _, _, _ -> }, onCrop = { _, _ -> },
                onRgbCurveOptions = { _, _, _ -> }, onWhiteBalanceGains = { _, _ -> },
                onWhiteBalanceSetting = { _, _ -> }, onWhiteBalanceTemperature = { _, _, _ -> },
                controlQuery = query, sessionId = state.sessionId, showHeader = false)
        }
    }

    private fun geometry(): Map<String, Rect> = buildMap {
        listOf("editor_preview_pane", "editor_tools_pane", "editor_tool_picker", "editor_tool_switch",
            "editor_tool_search_button", "editor_open", "editor_undo", "editor_redo", "editor_export",
            "editor_parameter_scroll").forEach { tag ->
            put(tag, compose.onNodeWithTag(tag).fetchSemanticsNode().boundsInRoot)
        }
        listOf(R.string.workspace_tab_adjustments, R.string.workspace_tab_detail,
            R.string.workspace_tab_geometry, R.string.workspace_tab_settings).forEach { label ->
            put("tab-$label", compose.onNodeWithText(context.getString(label)).fetchSemanticsNode().boundsInRoot)
        }
        put("input", compose.onNode(hasSetTextAction() and hasText("Exposure")).fetchSemanticsNode().boundsInRoot)
    }

    private fun activity() = ActivityLifecycleMonitorRegistry.getInstance().getActivitiesInStage(Stage.RESUMED).single()

    /** Software view-tree draw avoids the unavailable host PixelCopy frame callback. */
    private fun capture(): Bitmap {
        val bounds = compose.onRoot().fetchSemanticsNode().boundsInRoot
        return compose.runOnIdle {
            val content = activity().findViewById<View>(android.R.id.content)
            val bitmap = Bitmap.createBitmap(content.width, content.height, Bitmap.Config.ARGB_8888)
            content.draw(Canvas(bitmap))
            Bitmap.createBitmap(bitmap, bounds.left.roundToInt(), bounds.top.roundToInt(),
                bounds.width.roundToInt(), bounds.height.roundToInt())
        }
    }

    @Test fun theDefaultAndSavedChoiceSurviveANewContextAndInvalidPreferencesFallBack() {
        val preferences = context.getSharedPreferences("app-theme", Context.MODE_PRIVATE)
        preferences.edit().clear().commit()
        assertEquals(AppTheme.DESKTOP, AppThemes.selected(context))
        AppTheme.entries.forEach { choice ->
            AppThemes.select(context, choice)
            val otherContext = context.createConfigurationContext(context.resources.configuration)
            assertEquals(choice, AppThemes.selected(otherContext))
        }
        preferences.edit().putString("theme", "unknown-future-theme").commit()
        assertEquals(AppTheme.DESKTOP, AppThemes.selected(context))
    }

    @Test fun theThreeThemeOptionsSendOneChoiceAndDismissTheMenu() {
        var selected by mutableStateOf(AppTheme.DESKTOP)
        val changes = mutableListOf<AppTheme>()
        compose.setContent { DarktableTheme(theme = selected) {
            AppThemeMenu(selected, onSelect = { changes += it; selected = it })
        } }
        compose.onNodeWithTag("app_theme_picker").performClick()
        AppTheme.entries.forEach { choice ->
            compose.onNodeWithTag("app_theme_${choice.name}").assertIsDisplayed()
        }
        compose.onNodeWithTag("app_theme_OLED").performClick()
        compose.onNodeWithTag("app_theme_OLED").assertDoesNotExist()
        compose.onNodeWithTag("app_theme_picker").assertTextContains(context.getString(AppTheme.OLED.label))
        compose.runOnIdle { assertEquals(listOf(AppTheme.OLED), changes) }
    }

    @Test fun livePreferenceChangesKeepControlBoundsDraftAndTheOpenSession() {
        val current = ready()
        val selections = mutableListOf<String>()
        val enabled = mutableListOf<Pair<String, Boolean>>()
        val edits = mutableListOf<Triple<String, String, Double>>()
        AppThemes.select(context, AppTheme.DESKTOP)
        compose.setContent { DarktableTheme {
            Workspace(current, { selections += it }, { operation, value -> enabled += operation to value },
                { operation, name, value -> edits += Triple(operation, name, value) })
        } }
        val originalActivity = compose.runOnIdle { activity() }
        compose.onNode(hasSetTextAction() and hasText("Exposure")).performTextReplacement("0.5")
        val originalGeometry = geometry()
        val history = current.history
        val preview = current.preview
        val images = mutableListOf<Bitmap>()
        listOf(AppTheme.LIGHT, AppTheme.OLED, AppTheme.DESKTOP).forEach { choice ->
            compose.runOnIdle { AppThemes.select(context, choice) }
            compose.waitForIdle()
            assertEquals("Theme changes must not move the editor", originalGeometry, geometry())
            compose.onNode(hasSetTextAction() and hasText("Exposure")).assertTextContains("0.5")
            val captured = capture()
            images += captured
            val directory = File(System.getProperty("darktable.theme.capture.dir") ?: "build/reports/theme-preview")
            check(directory.isDirectory || directory.mkdirs())
            File(directory, "app-theme-${choice.tag}.png").outputStream().use {
                check(captured.compress(Bitmap.CompressFormat.PNG, 100, it))
            }
            compose.runOnIdle {
                assertSame("A theme choice must not recreate the Activity", originalActivity, activity())
                assertSame(history, current.history)
                assertSame(preview, current.preview)
                assertEquals("exposure", current.selectedModule)
                assertTrue(selections.isEmpty() && enabled.isEmpty() && edits.isEmpty())
            }
        }
        assertFalse("Light and OLED must actually draw different palettes", images[0].sameAs(images[1]))
        compose.onNodeWithText(context.getString(R.string.apply)).performClick()
        compose.runOnIdle { assertEquals(listOf(Triple("exposure", "exposure", .5)), edits) }
    }

    @Test fun oledDrawsPureBlackEmptyAreasAndLightTextKeepsReadableContrast() {
        var theme by mutableStateOf(AppTheme.OLED)
        val current = ready()
        compose.setContent { DarktableTheme(theme = theme) { Workspace(current) } }
        val image = capture()
        val root = compose.onRoot().fetchSemanticsNode().boundsInRoot
        listOf("editor_preview_pane", "editor_tools_pane").forEach { tag ->
            val bounds = compose.onNodeWithTag(tag).fetchSemanticsNode().boundsInRoot
            val x = (bounds.left - root.left + 2).roundToInt()
            val y = (bounds.center.y - root.top).roundToInt()
            assertEquals("OLED empty photo/tool areas must turn pixels off", android.graphics.Color.BLACK, image.getPixel(x, y))
        }
        val oled = appThemeScheme(AppTheme.OLED)
        val light = appThemeScheme(AppTheme.LIGHT)
        fun channel(value: Float): Double = if(value <= .04045f) value / 12.92 else ((value + .055) / 1.055).pow(2.4)
        fun luminance(color: androidx.compose.ui.graphics.Color): Double =
            .2126 * channel(color.red) + .7152 * channel(color.green) + .0722 * channel(color.blue)
        fun contrast(a: androidx.compose.ui.graphics.Color, b: androidx.compose.ui.graphics.Color): Double {
            val first = luminance(a); val second = luminance(b)
            return (max(first, second) + .05) / (min(first, second) + .05)
        }
        assertTrue(contrast(light.onSurface, light.surface) >= 4.5)
        assertTrue(contrast(light.onPrimaryContainer, light.primaryContainer) >= 4.5)
        assertTrue(contrast(oled.onSurface, oled.surface) >= 4.5)
        compose.runOnIdle { theme = AppTheme.LIGHT }
        compose.waitForIdle()
        assertFalse(image.sameAs(capture()))
    }

    @Test @SdkSuppress(minSdkVersion = 29)
    fun systemBarAppearanceFollowsLightAndOledWithoutRecreatingTheActivity() {
        var theme by mutableStateOf(AppTheme.LIGHT)
        val current = ready()
        compose.setContent { DarktableTheme(theme = theme) { Workspace(current) } }
        val original = compose.runOnIdle { activity() }
        compose.runOnIdle {
            val window = original.window
            val controller = WindowCompat.getInsetsController(window, window.decorView)
            assertTrue(controller.isAppearanceLightStatusBars)
            assertTrue(controller.isAppearanceLightNavigationBars)
            assertEquals(android.graphics.Color.rgb(250, 250, 250), (window.decorView.background as ColorDrawable).color)
            assertTrue(window.isNavigationBarContrastEnforced)
        }
        compose.runOnIdle { theme = AppTheme.OLED }
        compose.waitForIdle()
        compose.runOnIdle {
            assertSame(original, activity())
            val controller = WindowCompat.getInsetsController(original.window, original.window.decorView)
            assertFalse(controller.isAppearanceLightStatusBars)
            assertFalse(controller.isAppearanceLightNavigationBars)
            assertEquals(android.graphics.Color.BLACK, (original.window.decorView.background as ColorDrawable).color)
            assertFalse(original.window.isNavigationBarContrastEnforced)
        }
    }

    @Test fun themeSelectionIsReachableWithoutAnImageOrProcessingEngine() {
        var callbacks = 0
        compose.setContent { DarktableTheme {
            EditorWorkspace(EditorState.Empty, RuntimeAvailability(checking = false, available = false),
                { callbacks++ }, { callbacks++ }, { callbacks++ }, { callbacks++ }, {}, {},
                { callbacks++ }, { _, _ -> callbacks++ }) { _, _ -> }
        } }
        compose.onNodeWithText(context.getString(R.string.workspace_tab_settings)).performClick()
        compose.onNodeWithTag("app_theme_picker").performScrollTo().assertIsDisplayed().performClick()
        AppTheme.entries.forEach { choice -> compose.onNodeWithTag("app_theme_${choice.name}").assertIsDisplayed() }
        compose.runOnIdle { assertEquals(0, callbacks) }
    }
}
