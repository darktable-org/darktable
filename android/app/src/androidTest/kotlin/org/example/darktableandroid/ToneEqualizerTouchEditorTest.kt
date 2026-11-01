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
import androidx.compose.ui.test.assertIsEnabled
import androidx.compose.ui.test.assertIsNotEnabled
import androidx.compose.ui.test.click
import androidx.compose.ui.test.junit4.createComposeRule
import androidx.compose.ui.test.onNodeWithContentDescription
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

/** Touch callbacks and native feedback only; pixelpipe behavior is a native test. */
class ToneEqualizerTouchEditorTest {
    @get:Rule val compose = createComposeRule()
    private val context get() = InstrumentationRegistry.getInstrumentation().targetContext
    private fun sample() = ToneEqualizerSample(41L, "toneequal@3",
        Bitmap.createBitmap(32, 24, Bitmap.Config.ARGB_8888), .25f, .75f, -4.2, selectionId = 9)
    private data class Request(val sample: ToneEqualizerSample, val target: Double, val gesture: Long, val finished: Boolean)
    private fun sliderDescription() = context.getString(R.string.parameter_slider_description,
        context.getString(R.string.tone_touch_adjustment))

    @Test fun quickFineButtonsApplyImmediatelyAndAccumulateWithoutWaitingForAFrame() {
        val sample = sample()
        val edits = mutableListOf<Request>()
        compose.setContent { MaterialTheme {
            Column(Modifier.verticalScroll(rememberScrollState())) {
                ToneEqualizerTouchEditor(true, true, sample, {}, {},
                    { selected, target, gesture, finished -> edits += Request(selected, target, gesture, finished) })
            }
        } }
        compose.onNodeWithTag("tone_equalizer_brighter").performScrollTo().performClick().performClick()
        compose.runOnIdle {
            assertEquals(2, edits.size)
            assertEquals(-4.1, edits[0].target, 1e-12)
            assertEquals(-4.0, edits[1].target, 1e-12)
            assertTrue(edits.all { it.sample === sample && it.finished && it.gesture > 0 })
            assertTrue(edits[0].gesture != edits[1].gesture)
        }
        compose.onNodeWithTag("tone_equalizer_apply").assertDoesNotExist()
        compose.onNodeWithTag("tone_equalizer_cancel_draft").assertDoesNotExist()
        compose.onNodeWithText("Adjustment:", substring = true).assertDoesNotExist()
        compose.onNodeWithText(context.getString(R.string.tone_touch_sample, -4.0)).assertIsDisplayed()
    }

    @Test fun sliderStreamsAbsoluteTargetsAndFinishesOneGestureWithoutApply() {
        val edits = mutableListOf<Request>()
        compose.setContent { MaterialTheme {
            Column(Modifier.verticalScroll(rememberScrollState())) {
                ToneEqualizerTouchEditor(true, true, sample(), {}, {},
                    { selected, target, gesture, finished -> edits += Request(selected, target, gesture, finished) })
            }
        } }
        compose.onNodeWithContentDescription(sliderDescription()).performScrollTo().performTouchInput {
            down(center)
            moveTo(center + Offset(width * .1f, 0f))
            moveTo(center + Offset(width * .2f, 0f))
            up()
        }
        compose.runOnIdle {
            val moves = edits.filterNot { it.finished }
            assertTrue(moves.size >= 2)
            assertTrue(moves.map { it.target }.distinct().size >= 2)
            assertTrue(edits.map { it.gesture }.distinct().size == 1)
            assertTrue(edits.last().finished)
            assertEquals(moves.last().target, edits.last().target, 0.0)
        }
    }

    @Test fun aNativePreviewUpdateCannotResetAnActiveSliderGesture() {
        var sample by mutableStateOf(sample())
        val edits = mutableListOf<Request>()
        compose.setContent { MaterialTheme {
            Column(Modifier.verticalScroll(rememberScrollState())) {
                ToneEqualizerTouchEditor(true, true, sample, {}, {},
                    { selected, target, gesture, finished -> edits += Request(selected, target, gesture, finished) })
            }
        } }
        compose.onNodeWithContentDescription(sliderDescription()).performScrollTo().performTouchInput {
            down(center)
            moveTo(center + Offset(width * .15f, 0f))
        }
        var beforeFeedback = 0.0
        compose.runOnIdle {
            beforeFeedback = edits.last().target
            sample = sample.copy(preview = Bitmap.createBitmap(32, 24, Bitmap.Config.ARGB_8888),
                selectedExposure = -4.15, pendingExposure = -4.1, feedbackRevision = 1)
        }
        compose.onNodeWithText(context.getString(R.string.tone_touch_sample, beforeFeedback)).assertIsDisplayed()
        compose.onNodeWithContentDescription(sliderDescription()).performTouchInput {
            moveTo(center + Offset(width * .25f, 0f))
            up()
        }
        compose.runOnIdle {
            assertTrue(edits.map { it.gesture }.distinct().size == 1)
            assertTrue(edits.last().finished)
            assertTrue(edits.last().target > beforeFeedback)
        }
    }

    @Test fun selectedToneFollowsPendingThenActualNativeOutput() {
        var sample by mutableStateOf(sample())
        compose.setContent { MaterialTheme {
            Column(Modifier.verticalScroll(rememberScrollState())) {
                ToneEqualizerTouchEditor(true, true, sample, {}, {}, { _, _, _, _ -> })
            }
        } }
        compose.onNodeWithTag("tone_equalizer_brighter").performScrollTo().performClick()
        compose.runOnIdle { sample = sample.copy(pendingExposure = -4.1, feedbackRevision = 1) }
        compose.onNodeWithText(context.getString(R.string.tone_touch_sample, -4.1)).assertIsDisplayed()
        compose.runOnIdle { sample = sample.copy(selectedExposure = -4.13, pendingExposure = null, feedbackRevision = 2) }
        compose.onNodeWithText(context.getString(R.string.tone_touch_sample, -4.13)).assertIsDisplayed()
    }

    @Test fun rejectedFeedbackRollsBackEvenWhenPendingFrameWasConflated() {
        var sample by mutableStateOf(sample())
        compose.setContent { MaterialTheme {
            Column(Modifier.verticalScroll(rememberScrollState())) {
                ToneEqualizerTouchEditor(true, true, sample, {}, {}, { _, _, _, _ -> })
            }
        } }
        compose.onNodeWithTag("tone_equalizer_brighter").performScrollTo().performClick()
        compose.onNodeWithText(context.getString(R.string.tone_touch_sample, -4.1)).assertIsDisplayed()
        compose.runOnIdle { sample = sample.copy(feedbackRevision = 1) }
        compose.onNodeWithText(context.getString(R.string.tone_touch_sample, -4.2)).assertIsDisplayed()
    }

    @Test fun selectingCentreWorksWithoutPreciseTouchAndRequiresASampleBeforeAdjustment() {
        var centres = 0
        compose.setContent { MaterialTheme {
            ToneEqualizerTouchEditor(true, true, null, {}, { centres++ }, { _, _, _, _ -> })
        } }
        compose.onNodeWithText(context.getString(R.string.tone_touch_choose_point)).assertIsDisplayed()
        compose.onNodeWithTag("tone_equalizer_brighter").assertDoesNotExist()
        compose.onNodeWithTag("tone_equalizer_select_centre").performClick()
        compose.runOnIdle { assertEquals(1, centres) }
    }

    @Test fun unrelatedBusyStateDisablesSamplingAndAdjustmentActions() {
        compose.setContent { MaterialTheme {
            Column(Modifier.verticalScroll(rememberScrollState())) {
                ToneEqualizerTouchEditor(false, true, sample(), {}, {}, { _, _, _, _ -> })
            }
        } }
        listOf("tone_equalizer_image_mode", "tone_equalizer_select_centre", "tone_equalizer_brighter",
            "tone_equalizer_darker").forEach { tag ->
            compose.onNodeWithTag(tag).performScrollTo().assertIsNotEnabled()
        }
        compose.onNodeWithContentDescription(sliderDescription()).performScrollTo().assertIsNotEnabled()
    }

    @Test fun toneUpdatesKeepAdjustmentsEnabledWhileSelectionControlsAreLocked() {
        val edits = mutableListOf<Request>()
        compose.setContent { MaterialTheme {
            Column(Modifier.verticalScroll(rememberScrollState())) {
                ToneEqualizerTouchEditor(true, true, sample().copy(pendingExposure = -4.1), {}, {},
                    { selected, target, gesture, finished -> edits += Request(selected, target, gesture, finished) },
                    selectionEnabled = false)
            }
        } }
        compose.onNodeWithTag("tone_equalizer_image_mode").assertIsNotEnabled()
        compose.onNodeWithTag("tone_equalizer_select_centre").assertIsNotEnabled()
        compose.onNodeWithContentDescription(sliderDescription()).performScrollTo().assertIsEnabled()
        compose.onNodeWithTag("tone_equalizer_brighter").performScrollTo().performClick()
        compose.runOnIdle { assertEquals(-4.0, edits.single().target, 1e-12) }
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
