/* SPDX-License-Identifier: GPL-3.0-or-later */
package org.example.darktableandroid

import android.content.Context
import android.content.pm.ActivityInfo
import androidx.activity.ComponentActivity
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import androidx.compose.ui.test.assertIsDisplayed
import androidx.compose.ui.test.assertTextContains
import androidx.compose.ui.test.junit4.createComposeRule
import androidx.compose.ui.test.onNodeWithTag
import androidx.compose.ui.test.onNodeWithText
import androidx.compose.ui.test.performClick
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

@RunWith(RobolectricTestRunner::class)
@Config(sdk = [35], qualifiers = "en-rUS-w400dp-h800dp-mdpi")
@GraphicsMode(GraphicsMode.Mode.NATIVE)
class AppLanguageMenuTest {
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

    @Test fun theFiveLanguageChoicesInvokeOneCallbackAndCloseTheMenu() {
        var selected by mutableStateOf(AppLanguage.SYSTEM)
        val changes = mutableListOf<AppLanguage>()
        compose.setContent { DarktableTheme {
            AppLanguageMenu(selected, onSelect = { changes += it; selected = it })
        } }
        compose.onNodeWithTag("app_language_picker").performClick()
        AppLanguage.entries.forEach { choice ->
            compose.onNodeWithTag("app_language_${choice.name}").assertIsDisplayed()
        }
        compose.onNodeWithTag("app_language_LATIN_AMERICAN_SPANISH").performClick()
        compose.onNodeWithTag("app_language_LATIN_AMERICAN_SPANISH").assertDoesNotExist()
        compose.onNodeWithTag("app_language_picker")
            .assertTextContains(context.getString(R.string.app_language_es_419))
        compose.runOnIdle { assertEquals(listOf(AppLanguage.LATIN_AMERICAN_SPANISH), changes) }

        compose.onNodeWithTag("app_language_picker").performClick()
        compose.onNodeWithTag("app_language_SYSTEM").performClick()
        compose.onNodeWithTag("app_language_picker")
            .assertTextContains(context.getString(R.string.app_language_system))
        compose.runOnIdle { assertEquals(listOf(AppLanguage.LATIN_AMERICAN_SPANISH, AppLanguage.SYSTEM), changes) }
    }

    @Test fun languageSelectionIsReachableFromSettingsWithoutAnImageOrNativeEngine() {
        var callbacks = 0
        compose.setContent { DarktableTheme {
            EditorWorkspace(EditorState.Empty,
                RuntimeAvailability(checking = false, available = false, message = "Unavailable fixture"),
                { callbacks++ }, { callbacks++ }, { callbacks++ }, { callbacks++ }, {}, {},
                { callbacks++ }, { _, _ -> callbacks++ }) { _, _ -> }
        } }
        compose.onNodeWithText(context.getString(R.string.workspace_tab_settings)).performClick()
        compose.onNodeWithTag("app_language_picker").assertIsDisplayed().performClick()
        AppLanguage.entries.forEach { choice ->
            compose.onNodeWithTag("app_language_${choice.name}").assertIsDisplayed()
        }
        compose.runOnIdle { assertTrue("Opening the language menu must not edit or open a photo", callbacks == 0) }
    }
}
