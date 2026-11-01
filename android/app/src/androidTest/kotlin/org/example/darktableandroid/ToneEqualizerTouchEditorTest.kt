/* SPDX-License-Identifier: GPL-3.0-or-later */
package org.example.darktableandroid

import android.graphics.Bitmap
import android.net.Uri
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.test.assertIsDisplayed
import androidx.compose.ui.test.assertIsNotEnabled
import androidx.compose.ui.test.click
import androidx.compose.ui.test.junit4.createComposeRule
import androidx.compose.ui.test.onNodeWithTag
import androidx.compose.ui.test.onNodeWithText
import androidx.compose.ui.test.performClick
import androidx.compose.ui.test.performScrollTo
import androidx.compose.ui.test.performTouchInput
import androidx.compose.ui.test.swipe
import androidx.compose.ui.unit.dp
import androidx.test.platform.app.InstrumentationRegistry
import org.example.darktableandroid.nativecore.EditorModule
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Rule
import org.junit.Test

/** Touch and draft callbacks only; original-pixelpipe behavior is a native test. */
class ToneEqualizerTouchEditorTest {
    @get:Rule val compose = createComposeRule()
    private val context get() = InstrumentationRegistry.getInstrumentation().targetContext
    private fun sample() = ToneEqualizerSample(41L, "toneequal@3",
        Bitmap.createBitmap(32, 24, Bitmap.Config.ARGB_8888), .25f, .75f, -4.2)

    @Test fun fineAdjustmentsRemainDraftsUntilOneExplicitApply() {
        val sample = sample()
        val edits = mutableListOf<Pair<ToneEqualizerSample, Double>>()
        compose.setContent { MaterialTheme {
            Column(Modifier.verticalScroll(rememberScrollState())) {
                ToneEqualizerTouchEditor(true, true, sample, {}, {}, { selected, offset -> edits += selected to offset })
            }
        } }
        compose.onNodeWithTag("tone_equalizer_brighter").performScrollTo().performClick().performClick()
        compose.runOnIdle { assertTrue(edits.isEmpty()) }
        compose.onNodeWithTag("tone_equalizer_apply").performScrollTo().performClick()
        compose.runOnIdle {
            assertEquals(1, edits.size)
            assertTrue(edits.single().first === sample)
            assertEquals(.2, edits.single().second, 0.0)
        }
    }

    @Test fun cancelDraftDoesNotEditTheImage() {
        var edits = 0
        compose.setContent { MaterialTheme {
            Column(Modifier.verticalScroll(rememberScrollState())) {
                ToneEqualizerTouchEditor(true, true, sample(), {}, {}, { _, _ -> edits++ })
            }
        } }
        compose.onNodeWithTag("tone_equalizer_darker").performScrollTo().performClick()
        compose.onNodeWithTag("tone_equalizer_cancel_draft").performScrollTo().performClick()
        compose.onNodeWithTag("tone_equalizer_apply").assertIsNotEnabled()
        compose.runOnIdle { assertEquals(0, edits) }
    }

    @Test fun selectingCentreWorksWithoutPreciseTouchAndRequiresASampleBeforeAdjustment() {
        var centres = 0
        compose.setContent { MaterialTheme {
            ToneEqualizerTouchEditor(true, true, null, {}, { centres++ }, { _, _ -> })
        } }
        compose.onNodeWithText(context.getString(R.string.tone_touch_choose_point)).assertIsDisplayed()
        compose.onNodeWithTag("tone_equalizer_apply").assertDoesNotExist()
        compose.onNodeWithTag("tone_equalizer_select_centre").performClick()
        compose.runOnIdle { assertEquals(1, centres) }
    }

    @Test fun busyStateDisablesSamplingAndAdjustmentActions() {
        compose.setContent { MaterialTheme {
            Column(Modifier.verticalScroll(rememberScrollState())) {
                ToneEqualizerTouchEditor(false, true, sample(), {}, {}, { _, _ -> })
            }
        } }
        listOf("tone_equalizer_image_mode", "tone_equalizer_select_centre", "tone_equalizer_brighter",
            "tone_equalizer_darker", "tone_equalizer_apply").forEach { tag ->
            compose.onNodeWithTag(tag).performScrollTo().assertIsNotEnabled()
        }
    }

    @Test fun aNewNativeSampleResetsTheAdjustmentDraft() {
        var sample by mutableStateOf(sample())
        compose.setContent { MaterialTheme {
            Column(Modifier.verticalScroll(rememberScrollState())) {
                ToneEqualizerTouchEditor(true, true, sample, {}, {}, { _, _ -> })
            }
        } }
        compose.onNodeWithTag("tone_equalizer_brighter").performScrollTo().performClick()
        compose.runOnIdle { sample = sample.copy(preview = Bitmap.createBitmap(32, 24, Bitmap.Config.ARGB_8888)) }
        compose.onNodeWithTag("tone_equalizer_apply").performScrollTo().assertIsNotEnabled()
    }

    @Test fun letterboxTapsAndDragsDoNotSampleThePhoto() {
        val points = mutableListOf<ToneEqualizerPoint>()
        compose.setContent { MaterialTheme {
            Box(Modifier.size(300.dp, 200.dp)) { ToneEqualizerPhotoTarget(100, 200, true, null, { points += it }) }
        } }
        compose.onNodeWithTag("tone_equalizer_photo_target").performTouchInput {
            click(Offset(0f, center.y))
            swipe(center, center + Offset(0f, 40f))
        }
        compose.runOnIdle { assertTrue(points.isEmpty()) }
        compose.onNodeWithTag("tone_equalizer_photo_target").performTouchInput { click(center) }
        compose.runOnIdle { assertEquals(listOf(ToneEqualizerPoint(.5f, .5f)), points) }
    }

    @Test fun stalePreviewCannotArmToneSelectionAndOffersReadOnlyRetry() {
        val sample = sample()
        val state = EditorState.Ready(Uri.parse("content://tone-fixture/image.raw"), sample.sessionId, sample.preview,
            listOf(EditorModule(sample.module, "Tone equalizer", true, true, emptyList())), emptyList(),
            selectedModule = sample.module, previewCurrent = false)
        var retries = 0
        var samples = 0
        compose.setContent { MaterialTheme {
            EditorWorkspace(state, RuntimeAvailability(checking = false, available = true),
                onOpen = {}, onExport = {}, onUndo = {}, onRedo = {}, onCancel = {}, onRetry = {},
                onSelectModule = {}, onEnabled = { _, _ -> }, onRefreshPreview = { retries++ },
                onToneSample = { _, _, _ -> samples++ }) { _, _ -> Text("Tone controls") }
        } }
        compose.onNodeWithText(context.getString(R.string.tone_touch_stale_preview)).assertIsDisplayed()
        compose.onNodeWithTag("tone_equalizer_image_mode").assertIsNotEnabled()
        compose.onNodeWithTag("tone_equalizer_photo_target").assertDoesNotExist()
        compose.onNodeWithTag("editor_retry_preview").performClick()
        compose.runOnIdle { assertEquals(1, retries); assertEquals(0, samples) }
    }
}
