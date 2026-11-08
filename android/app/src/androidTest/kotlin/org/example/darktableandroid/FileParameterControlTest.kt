/* SPDX-License-Identifier: GPL-3.0-or-later */
package org.example.darktableandroid

import android.net.Uri
import androidx.activity.compose.LocalActivityResultRegistryOwner
import androidx.activity.result.ActivityResultRegistry
import androidx.activity.result.ActivityResultRegistryOwner
import androidx.activity.result.contract.ActivityResultContract
import androidx.compose.material3.MaterialTheme
import androidx.compose.runtime.CompositionLocalProvider
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableLongStateOf
import androidx.compose.runtime.setValue
import androidx.compose.ui.test.assertCountEquals
import androidx.compose.ui.test.hasSetTextAction
import androidx.compose.ui.test.junit4.createComposeRule
import androidx.compose.ui.test.onNodeWithText
import androidx.compose.ui.test.performClick
import androidx.core.app.ActivityOptionsCompat
import androidx.test.platform.app.InstrumentationRegistry
import org.example.darktableandroid.nativecore.ModuleTextParameter
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotNull
import org.junit.Rule
import org.junit.Test

class FileParameterControlTest {
    @get:Rule val compose = createComposeRule()
    private val context get() = InstrumentationRegistry.getInstrumentation().targetContext

    @Test fun documentResultRetainsTheImageSessionCapturedBeforeThePickerOpened() {
        val registry = object : ActivityResultRegistry() {
            var pending: Int? = null
            override fun <I, O> onLaunch(requestCode: Int, contract: ActivityResultContract<I, O>, input: I,
                                          options: ActivityOptionsCompat?) {
                pending = requestCode
            }
        }
        val owner = object : ActivityResultRegistryOwner {
            override val activityResultRegistry: ActivityResultRegistry = registry
        }
        var sessionId by mutableLongStateOf(41L)
        var selected: Pair<Uri, Long>? = null
        val parameter = ModuleTextParameter("file.import", "LUT file", "/private/immutable/test.cube", 1023,
            isFile = true, extensions = listOf("cube", "3dl", "png"))
        compose.setContent { CompositionLocalProvider(LocalActivityResultRegistryOwner provides owner) {
            MaterialTheme { FileParameterControl(parameter, true, sessionId) { uri, expected -> selected = uri to expected } }
        } }
        compose.onNodeWithText("test.cube").assertExists()
        compose.onNodeWithText("/private/immutable/test.cube").assertDoesNotExist()
        compose.onAllNodes(hasSetTextAction()).assertCountEquals(0)
        compose.onNodeWithText(context.getString(R.string.tool_file_choose)).performClick()
        compose.runOnIdle { assertNotNull(registry.pending); sessionId = 42L }
        compose.waitForIdle()
        val document = Uri.parse("content://test-provider/chosen.cube")
        compose.runOnIdle { registry.dispatchResult(requireNotNull(registry.pending), document) }
        compose.runOnIdle { assertEquals(document to 41L, selected) }
    }
}
