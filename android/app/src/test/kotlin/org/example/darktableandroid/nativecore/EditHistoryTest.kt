/* SPDX-License-Identifier: GPL-3.0-or-later */
package org.example.darktableandroid.nativecore

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Test

class EditHistoryTest {
    @Test fun unavailableDirectionsCannotEscapeNativeHistoryBoundaries() {
        assertNull(checkedEditHistory(true, false, 0, 2))
        assertNull(checkedEditHistory(false, true, 2, 2))
        assertNull(checkedEditHistory(false, false, -1, 2))
        assertNull(checkedEditHistory(false, false, 3, 2))
        assertNull(checkedEditHistory(false, false, 0, -1))
    }

    @Test fun nativePermissionsRemainAuthoritativeInsideTheHistory() {
        // Positions alone must not invent a capability withheld by the owner.
        val restricted = checkedEditHistory(false, false, 1, 3)!!
        assertFalse(restricted.canUndo)
        assertFalse(restricted.canRedo)
        assertEquals(EditHistory(true, true, 1, 3), checkedEditHistory(true, true, 1, 3))
        assertEquals(EditHistory(false, false, 0, 0), checkedEditHistory(false, false, 0, 0))
    }
}
