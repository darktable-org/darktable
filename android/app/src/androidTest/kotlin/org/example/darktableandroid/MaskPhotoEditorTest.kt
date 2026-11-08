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
import androidx.compose.ui.test.assertIsEnabled
import androidx.compose.ui.test.assertIsNotEnabled
import androidx.compose.ui.test.assertIsSelected
import androidx.compose.ui.test.click
import androidx.compose.ui.test.junit4.createComposeRule
import androidx.compose.ui.test.onNodeWithTag
import androidx.compose.ui.test.performClick
import androidx.compose.ui.test.performScrollTo
import androidx.compose.ui.test.performTouchInput
import androidx.compose.ui.test.swipe
import androidx.compose.ui.unit.dp
import org.example.darktableandroid.nativecore.EditorModule
import org.example.darktableandroid.nativecore.MaskPhotoForm
import org.example.darktableandroid.nativecore.MaskPhotoPoint
import org.example.darktableandroid.nativecore.MaskPhotoPoints
import org.example.darktableandroid.nativecore.ModuleParameter
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Rule
import org.junit.Test

class MaskPhotoEditorTest {
    @get:Rule val compose = createComposeRule()
    private val bitmap = Bitmap.createBitmap(32, 24, Bitmap.Config.ARGB_8888)
    private val position = MaskPhotoPoint(.25f, .75f, true)
    private fun points() = MaskPhotoPoints(listOf(
        MaskPhotoForm(7, "Circle 7", "circle", position, MaskPhotoPoint(.6f, .2f, true)),
        MaskPhotoForm(9, "Clone brush 9", "brush", null, MaskPhotoPoint(null, null, false)),
    ))
    private fun state(module: String = "exposure@2", previewCurrent: Boolean = true, enabled: Boolean = true) =
        EditorState.Ready(Uri.parse("content://mask-fixture/image.raw"), 51L, bitmap,
            listOf(EditorModule(module, "Exposure", enabled, true,
                listOf(ModuleParameter("mask.7.x", "Circle 7 · x", "float", .1, 0.0, 1.0, emptyList())))),
            emptyList(), selectedModule = module, previewCurrent = previewCurrent)

    @Test fun selectingAFormAndCloneSourceOnlyChangesTheTarget() {
        var formId by mutableStateOf<Int?>(7)
        var source by mutableStateOf(false)
        var modeChanges = 0
        compose.setContent { MaterialTheme {
            Column(Modifier.verticalScroll(rememberScrollState())) {
                MaskPhotoEditor(true, true, points(), formId, source, { modeChanges++ },
                    { formId = it }, { source = it })
            }
        } }
        compose.onNodeWithTag("mask_photo_source").performScrollTo().performClick().assertIsSelected()
        compose.onNodeWithTag("mask_photo_form_picker").performScrollTo().performClick()
        compose.onNodeWithTag("mask_photo_form_9").performClick()
        compose.onNodeWithTag("mask_photo_center").assertDoesNotExist()
        compose.runOnIdle { assertEquals(9, formId); assertTrue(source); assertEquals(0, modeChanges) }
    }

    @Test fun photoTapEmitsOneAtomicXYWithTheExactTargetAndRejectsLetterboxAndDrag() {
        val target = MaskPhotoTarget(51, "exposure@2", bitmap, 7, false)
        val placed = mutableListOf<Triple<MaskPhotoTarget, Float, Float>>()
        compose.setContent { MaterialTheme {
            Box(Modifier.size(300.dp, 200.dp)) {
                MaskPhotoTargetOverlay(100, 200, true, target, position) { lease, x, y -> placed += Triple(lease, x, y) }
            }
        } }
        compose.onNodeWithTag("mask_photo_target").performTouchInput {
            click(Offset(0f, center.y))
            swipe(center, center + Offset(0f, 40f))
        }
        compose.runOnIdle { assertTrue(placed.isEmpty()) }
        compose.onNodeWithTag("mask_photo_target").performTouchInput { click(center) }
        compose.runOnIdle { assertEquals(listOf(Triple(target, .5f, .5f)), placed) }
    }

    @Test fun disabledTargetAndInvisibleNativePointCannotPlaceOrShowAMarker() {
        var enabled by mutableStateOf(false)
        var point by mutableStateOf(MaskPhotoPoint(1.2f, .5f, false))
        var placements = 0
        val target = MaskPhotoTarget(51, "exposure@2", bitmap, 7, true)
        compose.setContent { MaterialTheme {
            Box(Modifier.size(200.dp)) {
                MaskPhotoTargetOverlay(200, 200, enabled, target, point) { _, _, _ -> placements++ }
            }
        } }
        compose.onNodeWithTag("mask_photo_marker").assertDoesNotExist()
        compose.onNodeWithTag("mask_photo_target").performTouchInput { click(center) }
        compose.runOnIdle { assertEquals(0, placements); enabled = true; point = MaskPhotoPoint(Float.NaN, .5f, true) }
        compose.onNodeWithTag("mask_photo_marker").assertDoesNotExist()
        compose.onNodeWithTag("mask_photo_target").performClick()
        compose.runOnIdle { assertEquals(1, placements) }
    }

    @Test fun explicitModeLoadsReadOnlyThenPlacesTheSelectedCloneSource() {
        val ready = state()
        var selection by mutableStateOf<MaskPhotoSelection?>(null)
        val loads = mutableListOf<String>()
        val placements = mutableListOf<MaskPhotoTarget>()
        compose.setContent { MaterialTheme {
            EditorWorkspace(ready, RuntimeAvailability(checking = false, available = true),
                onOpen = {}, onExport = {}, onUndo = {}, onRedo = {}, onCancel = {}, onRetry = {},
                onSelectModule = {}, onEnabled = { _, _ -> }, maskPhotoSelection = selection,
                onLoadMaskPhoto = { loads += it; selection = MaskPhotoSelection(ready.sessionId, it, bitmap, points()) },
                onPlaceMaskPhoto = { target, _, _ -> placements += target }) { _, _ -> Text("Mask controls") }
        } }
        compose.onNodeWithTag("mask_photo_target").assertDoesNotExist()
        compose.onNodeWithTag("mask_photo_mode").performScrollTo().performClick()
        compose.runOnIdle { assertEquals(listOf("exposure@2"), loads); assertTrue(placements.isEmpty()) }
        compose.onNodeWithTag("mask_photo_source").performScrollTo().performClick()
        compose.onNodeWithTag("mask_photo_target").performTouchInput { click(center) }
        compose.runOnIdle {
            assertEquals(listOf(MaskPhotoTarget(51, "exposure@2", bitmap, 7, true)), placements)
        }
    }

    @Test fun stalePreviewAndDisabledModuleCannotArmPlacement() {
        var ready by mutableStateOf(state(previewCurrent = false))
        var loads = 0
        compose.setContent { MaterialTheme {
            EditorWorkspace(ready, RuntimeAvailability(checking = false, available = true),
                onOpen = {}, onExport = {}, onUndo = {}, onRedo = {}, onCancel = {}, onRetry = {},
                onSelectModule = {}, onEnabled = { _, _ -> }, onLoadMaskPhoto = { loads++ }) { _, _ -> Text("Mask controls") }
        } }
        compose.onNodeWithTag("mask_photo_mode").performScrollTo().assertIsNotEnabled()
        compose.runOnIdle { ready = state(enabled = false) }
        compose.onNodeWithTag("mask_photo_mode").performScrollTo().assertIsNotEnabled()
        compose.onNodeWithTag("mask_photo_target").assertDoesNotExist()
        compose.runOnIdle { assertEquals(0, loads) }
    }

    @Test fun projectionsForAnotherPreviewSessionOrInstanceCannotPlaceMasks() {
        val ready = state()
        var selection by mutableStateOf(MaskPhotoSelection(52, "exposure@2", bitmap, points()))
        var placements = 0
        compose.setContent { MaterialTheme {
            EditorWorkspace(ready, RuntimeAvailability(checking = false, available = true),
                onOpen = {}, onExport = {}, onUndo = {}, onRedo = {}, onCancel = {}, onRetry = {},
                onSelectModule = {}, onEnabled = { _, _ -> }, maskPhotoSelection = selection,
                onPlaceMaskPhoto = { _, _, _ -> placements++ }) { _, _ -> Text("Mask controls") }
        } }
        compose.onNodeWithTag("mask_photo_mode").performScrollTo().performClick()
        compose.onNodeWithTag("mask_photo_target").performTouchInput { click(center) }
        compose.runOnIdle { selection = selection.copy(sessionId = 51, module = "exposure@4") }
        compose.onNodeWithTag("mask_photo_target").performTouchInput { click(center) }
        compose.runOnIdle {
            selection = selection.copy(module = "exposure@2", preview = Bitmap.createBitmap(32, 24, Bitmap.Config.ARGB_8888))
        }
        compose.onNodeWithTag("mask_photo_target").performTouchInput { click(center) }
        compose.onNodeWithTag("mask_photo_marker").assertDoesNotExist()
        compose.runOnIdle { assertEquals(0, placements) }
    }

    @Test fun busyDisablesTapsAndANewPlacementFrameKeepsTheChosenCloneSource() {
        val initial = state()
        var ready by mutableStateOf(initial)
        var editorState by mutableStateOf<EditorState>(initial)
        var selection by mutableStateOf(MaskPhotoSelection(51, "exposure@2", bitmap, points()))
        val placements = mutableListOf<MaskPhotoTarget>()
        compose.setContent { MaterialTheme {
            EditorWorkspace(editorState, RuntimeAvailability(checking = false, available = true),
                onOpen = {}, onExport = {}, onUndo = {}, onRedo = {}, onCancel = {}, onRetry = {},
                onSelectModule = {}, onEnabled = { _, _ -> }, maskPhotoSelection = selection,
                onPlaceMaskPhoto = { target, _, _ -> placements += target }) { _, _ -> Text("Mask controls") }
        } }
        compose.onNodeWithTag("mask_photo_mode").performScrollTo().performClick()
        compose.onNodeWithTag("mask_photo_form_picker").performScrollTo().performClick()
        compose.onNodeWithTag("mask_photo_form_9").performClick()
        compose.runOnIdle { editorState = EditorState.Working(EditorState.Stage.EDITING, ready) }
        compose.onNodeWithTag("mask_photo_mode").performScrollTo().assertIsNotEnabled()
        compose.onNodeWithTag("mask_photo_target").performTouchInput { click(center) }
        compose.runOnIdle {
            assertTrue(placements.isEmpty())
            val nextPreview = Bitmap.createBitmap(32, 24, Bitmap.Config.ARGB_8888)
            ready = ready.copy(preview = nextPreview)
            selection = selection.copy(preview = nextPreview)
            editorState = ready
        }
        compose.onNodeWithTag("mask_photo_source").performScrollTo().assertIsSelected()
        compose.onNodeWithTag("mask_photo_target").performTouchInput { click(center) }
        compose.runOnIdle {
            assertEquals(listOf(MaskPhotoTarget(51, "exposure@2", ready.preview, 9, true)), placements)
        }
    }

    @Test fun clearingALoadedProjectionDisarmsPhotoPlacement() {
        val ready = state()
        var selection by mutableStateOf<MaskPhotoSelection?>(MaskPhotoSelection(51, "exposure@2", bitmap, points()))
        compose.setContent { MaterialTheme {
            EditorWorkspace(ready, RuntimeAvailability(checking = false, available = true),
                onOpen = {}, onExport = {}, onUndo = {}, onRedo = {}, onCancel = {}, onRetry = {},
                onSelectModule = {}, onEnabled = { _, _ -> }, maskPhotoSelection = selection) { _, _ -> Text("Mask controls") }
        } }
        compose.onNodeWithTag("mask_photo_mode").performScrollTo().performClick()
        compose.onNodeWithTag("mask_photo_marker").assertExists()
        compose.runOnIdle { selection = null }
        compose.onNodeWithTag("mask_photo_target").assertDoesNotExist()
        compose.onNodeWithTag("mask_photo_mode").assertIsEnabled()
    }

    @Test fun toneAndMaskPhotoModesDisarmEachOtherForAMaskedToneEqualizer() {
        val ready = state(module = "toneequal@3")
        compose.setContent { MaterialTheme {
            EditorWorkspace(ready, RuntimeAvailability(checking = false, available = true),
                onOpen = {}, onExport = {}, onUndo = {}, onRedo = {}, onCancel = {}, onRetry = {},
                onSelectModule = {}, onEnabled = { _, _ -> },
                maskPhotoSelection = MaskPhotoSelection(51, "toneequal@3", bitmap, points())) { _, _ -> Text("Mask controls") }
        } }
        compose.onNodeWithTag("tone_equalizer_image_mode").performScrollTo().performClick()
        compose.onNodeWithTag("tone_equalizer_photo_target").assertExists()
        compose.onNodeWithTag("mask_photo_mode").performScrollTo().performClick()
        compose.onNodeWithTag("mask_photo_target").assertExists()
        compose.onNodeWithTag("tone_equalizer_photo_target").assertDoesNotExist()
        compose.onNodeWithTag("tone_equalizer_image_mode").performScrollTo().performClick()
        compose.onNodeWithTag("tone_equalizer_photo_target").assertExists()
        compose.onNodeWithTag("mask_photo_target").assertDoesNotExist()
        compose.onNodeWithTag("tone_equalizer_image_mode").assertIsEnabled()
    }
}
