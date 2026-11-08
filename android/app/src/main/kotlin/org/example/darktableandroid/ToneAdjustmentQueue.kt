/* SPDX-License-Identifier: GPL-3.0-or-later */
package org.example.darktableandroid

internal data class ToneAdjustmentRequest(
    val selectionId: Long,
    val target: Double,
    val gestureId: Long,
    val finished: Boolean,
    val sequence: Long,
)

/** Main-thread single-flight queue for absolute tone targets, rather than deltas. */
internal class ToneAdjustmentQueue {
    private var selectionId: Long? = null
    private var nextSequence = 0L
    private var pending: ToneAdjustmentRequest? = null
    private var inFlight: ToneAdjustmentRequest? = null
    private var completed: ToneAdjustmentRequest? = null

    val hasPending: Boolean get() = pending != null
    val pendingTarget: Double? get() = pending?.target ?: inFlight?.target

    fun reset(selectionId: Long) {
        clear()
        this.selectionId = selectionId
    }

    fun clear() {
        selectionId = null
        pending = null
        inFlight = null
        completed = null
    }

    /** True when a new request was queued. A duplicate finish never repeats work. */
    fun offer(selectionId: Long, target: Double, gestureId: Long, finished: Boolean): Boolean {
        if(this.selectionId != selectionId || !target.isFinite() || gestureId <= 0) return false
        fun ToneAdjustmentRequest.matches() = this.gestureId == gestureId && this.target == target

        // A release can repeat the target already being rendered, even after
        // newer motion arrived. Finish that gesture without rewinding its target.
        if(finished && (inFlight?.matches() == true || completed?.matches() == true)) {
            pending = pending?.let { if(it.gestureId == gestureId) it.copy(finished = true) else it }
            inFlight = inFlight?.let { if(it.matches()) it.copy(finished = true) else it }
            return false
        }
        if(pending?.matches() == true) {
            if(finished) pending = pending?.copy(finished = true)
            return false
        }
        if(pending == null && (inFlight?.matches() == true || completed?.matches() == true)) return false

        pending = ToneAdjustmentRequest(selectionId, target, gestureId, finished, ++nextSequence)
        return true
    }

    /** Only one native operation may consume the queue at a time. */
    fun take(): ToneAdjustmentRequest? {
        if(inFlight != null) return null
        return pending?.also { pending = null; inFlight = it }
    }

    /** An older selection or operation cannot settle a newer operation. */
    fun complete(request: ToneAdjustmentRequest): Boolean {
        val active = inFlight ?: return false
        if(selectionId != request.selectionId || active.sequence != request.sequence) return false
        completed = active
        inFlight = null
        return true
    }
}
