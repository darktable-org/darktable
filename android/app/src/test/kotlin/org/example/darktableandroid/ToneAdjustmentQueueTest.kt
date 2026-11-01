/* SPDX-License-Identifier: GPL-3.0-or-later */
package org.example.darktableandroid

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class ToneAdjustmentQueueTest {
    @Test fun rapidAbsoluteTargetsCoalesceToTheLatestValueAndGesture() {
        val queue = ToneAdjustmentQueue().apply { reset(41) }
        assertTrue(queue.offer(41, .1, 1, true))
        assertTrue(queue.offer(41, .2, 2, true))
        assertTrue(queue.offer(41, .3, 3, true))
        assertTrue(queue.hasPending)
        assertEquals(.3, queue.pendingTarget!!, 0.0)
        val request = queue.take()!!
        assertEquals(.3, request.target, 0.0)
        assertEquals(3L, request.gestureId)
        assertTrue(request.finished)
        assertFalse(queue.hasPending)
        assertNull(queue.take())
        assertTrue(queue.complete(request))
        assertNull(queue.pendingTarget)
    }

    @Test fun aTargetArrivingDuringRenderingSurvivesCompletionOfTheOlderRequest() {
        val queue = ToneAdjustmentQueue().apply { reset(41) }
        queue.offer(41, .1, 1, false)
        val rendering = queue.take()!!
        queue.offer(41, .2, 1, false)
        queue.offer(41, .3, 1, true)
        assertEquals(.3, queue.pendingTarget!!, 0.0)
        assertNull(queue.take())
        assertTrue(queue.complete(rendering))
        val final = queue.take()!!
        assertEquals(.3, final.target, 0.0)
        assertTrue(final.finished)
        assertFalse(queue.complete(rendering))
        assertEquals(.3, queue.pendingTarget!!, 0.0)
        assertTrue(queue.complete(final))
    }

    @Test fun releaseRepeatingAnOlderTargetDoesNotRewindTheLatestQueuedMotion() {
        val queue = ToneAdjustmentQueue().apply { reset(41) }
        queue.offer(41, .1, 1, false)
        val rendering = queue.take()!!
        queue.offer(41, .3, 1, false)
        assertFalse(queue.offer(41, .1, 1, true))
        assertEquals(.3, queue.pendingTarget!!, 0.0)
        queue.complete(rendering)
        val final = queue.take()!!
        assertEquals(.3, final.target, 0.0)
        assertTrue(final.finished)
        queue.complete(final)
        assertFalse(queue.offer(41, .3, 1, true))
        assertFalse(queue.hasPending)
    }

    @Test fun movingBackToAnEarlierValueRemainsAnAuthoritativeTarget() {
        val queue = ToneAdjustmentQueue().apply { reset(41) }
        queue.offer(41, .1, 1, false)
        val rendering = queue.take()!!
        queue.offer(41, .3, 1, false)
        assertTrue(queue.offer(41, .1, 1, false))
        queue.complete(rendering)
        assertEquals(.1, queue.take()!!.target, 0.0)
    }

    @Test fun duplicatePendingFinishIsMergedWithoutAnotherNativeRequest() {
        val queue = ToneAdjustmentQueue().apply { reset(41) }
        queue.offer(41, .2, 1, false)
        assertFalse(queue.offer(41, .2, 1, true))
        val request = queue.take()!!
        assertTrue(request.finished)
        assertFalse(queue.offer(41, .2, 1, true))
        queue.complete(request)
        assertFalse(queue.offer(41, .2, 1, true))
        assertFalse(queue.hasPending)
        assertTrue(queue.offer(41, .2, 2, true))
    }

    @Test fun aNewSelectionRejectsOldEventsAndOldCompletions() {
        val queue = ToneAdjustmentQueue().apply { reset(41) }
        queue.offer(41, .1, 1, false)
        val old = queue.take()!!
        queue.reset(42)
        assertFalse(queue.offer(41, .4, 1, true))
        assertTrue(queue.offer(42, -.1, 2, true))
        val current = queue.take()!!
        assertFalse(queue.complete(old))
        assertEquals(-.1, queue.pendingTarget!!, 0.0)
        assertTrue(queue.complete(current))
        queue.clear()
        assertFalse(queue.offer(42, .2, 3, true))
        assertNull(queue.take())
    }

    @Test fun invalidInputCannotReplaceThePendingTarget() {
        val queue = ToneAdjustmentQueue().apply { reset(41) }
        queue.offer(41, .2, 1, false)
        for(value in listOf(Double.NaN, Double.POSITIVE_INFINITY, Double.NEGATIVE_INFINITY))
            assertFalse(queue.offer(41, value, 1, false))
        assertFalse(queue.offer(41, .3, 0, false))
        assertFalse(queue.offer(41, .3, -1, false))
        assertFalse(queue.offer(42, .3, 1, false))
        assertEquals(.2, queue.take()!!.target, 0.0)
    }

    @Test fun resettingTheSameLeaseStillInvalidatesEarlierCompletions() {
        val queue = ToneAdjustmentQueue().apply { reset(41) }
        queue.offer(41, .1, 1, false)
        val old = queue.take()!!
        queue.reset(41)
        queue.offer(41, .2, 2, true)
        val current = queue.take()!!
        assertFalse(queue.complete(old))
        assertEquals(.2, queue.pendingTarget!!, 0.0)
        assertTrue(queue.complete(current))
    }
}
