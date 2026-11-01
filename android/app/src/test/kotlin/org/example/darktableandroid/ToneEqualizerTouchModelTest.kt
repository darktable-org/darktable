/* SPDX-License-Identifier: GPL-3.0-or-later */
package org.example.darktableandroid

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test

class ToneEqualizerTouchModelTest {
    @Test fun portraitPhotoRejectsHorizontalLetterboxingAndMapsImageCoordinates() {
        val bounds = fittedPreviewBounds(400f, 200f, 100, 200)!!
        assertEquals(150f, bounds.left)
        assertEquals(0f, bounds.top)
        assertNull(bounds.pointAt(149f, 100f))
        assertNull(bounds.pointAt(250f, 100f))
        assertEquals(ToneEqualizerPoint(.5f, .5f), bounds.pointAt(200f, 100f))
        assertEquals(ToneEqualizerPoint(0f, 0f), bounds.pointAt(150f, 0f))
    }

    @Test fun landscapePhotoRejectsVerticalLetterboxingAndScalesInsideContentBox() {
        val bounds = fittedPreviewBounds(200f, 400f, 400, 200)!!
        assertEquals(0f, bounds.left)
        assertEquals(150f, bounds.top)
        assertNull(bounds.pointAt(100f, 149f))
        assertNull(bounds.pointAt(100f, 250f))
        assertEquals(ToneEqualizerPoint(.25f, .75f), bounds.pointAt(50f, 225f))
    }

    @Test fun invalidSizesAndNonfiniteTapsCannotBecomeNativeCoordinates() {
        assertNull(fittedPreviewBounds(0f, 400f, 400, 200))
        assertNull(fittedPreviewBounds(200f, Float.NaN, 400, 200))
        assertNull(fittedPreviewBounds(200f, 400f, 0, 200))
        val bounds = fittedPreviewBounds(200f, 200f, 200, 200)!!
        assertNull(bounds.pointAt(Float.NaN, 100f))
        assertNull(bounds.pointAt(100f, Float.POSITIVE_INFINITY))
        assertNull(bounds.pointAt(-1f, 100f))
    }

    @Test fun adjustmentDraftIsFiniteBoundedAndStableForFineSteps() {
        assertNull(toneEqualizerDraftOffset(Double.NaN))
        assertNull(toneEqualizerDraftOffset(Double.POSITIVE_INFINITY))
        assertEquals(2.0, toneEqualizerDraftOffset(3.0)!!, 0.0)
        assertEquals(-2.0, toneEqualizerDraftOffset(-3.0)!!, 0.0)
        assertEquals(.3, toneEqualizerDraftOffset(.1 + .1 + .1)!!, 0.0)
        assertEquals(-.15, toneEqualizerDraftOffset(-.151)!!, 0.0)
    }
}
