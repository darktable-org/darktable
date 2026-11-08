/* SPDX-License-Identifier: GPL-3.0-or-later */
package org.example.darktableandroid.nativecore

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test

class CurveResetMetadataTest {
    private fun curve(reset: String?) = parseModules("""[{
        "operation":"colorzones", "name":"color zones", "enabled":true,
        "curves":[{"channel":0,"label":"Lightness","editable":true,
          "interpolation":1,"maxNodes":20,"minSpacing":0.000001,
          "nodes":[{"x":0,"y":0.2},{"x":1,"y":0.8}],
          "samples":[{"x":0,"y":0.2},{"x":1,"y":0.8}]
          ${reset?.let { ",\"resetNodes\":$it" } ?: ""}
        }]
    }]""").single().curves.single()

    @Test fun nativeNeutralResetCanBeHorizontalInsteadOfIdentity() {
        assertEquals(listOf(CurvePoint(0f, 0.5f), CurvePoint(1f, 0.5f)),
            curve("""[{"x":0,"y":0.5},{"x":1,"y":0.5}]""").resetNodes)
    }

    @Test fun oldRuntimeOrMalformedOptionalResetDoesNotDiscardTheCurve() {
        assertNull(curve(null).resetNodes)
        val malformed = curve("""[{"x":0.5,"y":0.5},{"x":0.5,"y":0.5}]""")
        assertNull(malformed.resetNodes)
        assertEquals(2, malformed.nodes.size)
    }
}
