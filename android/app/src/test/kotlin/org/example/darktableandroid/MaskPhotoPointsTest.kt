/* SPDX-License-Identifier: GPL-3.0-or-later */
package org.example.darktableandroid

import org.example.darktableandroid.nativecore.parseMaskPhotoPoints
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class MaskPhotoPointsTest {
    @Test fun nativeProjectionKeepsFormIdsTypesAndCloneSourceCapabilities() {
        val points = requireNotNull(parseMaskPhotoPoints("""{"forms":[
            {"id":27,"label":"circle 27","type":"circle","center":{"x":0.2,"y":0.7,"visible":true},
             "source":{"x":0.1,"y":0.8,"visible":true}},
            {"id":94,"label":"clone brush","type":"brush","source":{"x":0.4,"y":0.6,"visible":true}}
        ]}"""))
        assertEquals(listOf(27, 94), points.forms.map { it.id })
        assertEquals("circle", points.forms.first().type)
        assertEquals(.2f, points.forms.first().center!!.x)
        assertEquals(.8f, points.forms.first().source!!.y)
        assertTrue(points.forms.first().center!!.visible)
        assertNull(points.forms.last().center)
        assertEquals("clone brush", points.forms.last().label)
    }

    @Test fun invisibleAndUnavailableProjectionsRemainPlaceableWithoutInventingMarkers() {
        val forms = requireNotNull(parseMaskPhotoPoints("""{"forms":[
            {"id":1,"label":"offscreen","type":"ellipse","center":{"x":1.3,"y":0.5,"visible":false}},
            {"id":2,"label":"unprojectable","type":"gradient","center":{"x":null,"y":null,"visible":false}},
            {"id":3,"label":"invalid visibility","type":"circle","center":{"x":1,"y":0.5,"visible":true}},
            {"id":4,"label":"invalid coordinate","type":"circle","center":{"x":"0.2","y":0.5,"visible":true}}
        ]}""")).forms
        assertEquals(4, forms.size)
        assertTrue(forms.all { it.center?.visible == false })
        assertEquals(1.3f, forms.first().center!!.x)
        assertNull(forms[1].center!!.x)
        assertNull(forms[3].center!!.x)
    }

    @Test fun malformedEnvelopeIsUnavailableAndEmptyNativeFormsAreValid() {
        listOf("", "not json", "[]", "{}", "{\"forms\":null}", "{\"forms\":{}}").forEach {
            assertNull(it, parseMaskPhotoPoints(it))
        }
        assertEquals(emptyList<Any>(), requireNotNull(parseMaskPhotoPoints("{\"forms\":[]}")).forms)
    }

    @Test fun malformedFormsAndCoercedIdentifiersNeverBecomePlacementTargets() {
        val forms = requireNotNull(parseMaskPhotoPoints("""{"forms":[
            null, 3,
            {"id":"7","label":"string ID","type":"circle","center":{"x":0.5,"y":0.5,"visible":true}},
            {"id":7.5,"label":"fraction ID","type":"circle","center":{"x":0.5,"y":0.5,"visible":true}},
            {"id":0,"label":"zero ID","type":"circle","center":{"x":0.5,"y":0.5,"visible":true}},
            {"id":2147483648,"label":"overflow ID","type":"circle","center":{"x":0.5,"y":0.5,"visible":true}},
            {"id":8,"label":true,"type":"circle","center":{"x":0.5,"y":0.5,"visible":true}},
            {"id":9,"label":"future form","type":"future","center":{"x":0.5,"y":0.5,"visible":true}},
            {"id":10,"label":"missing point","type":"path"},
            {"id":11,"label":"coerced bool","type":"circle","center":{"x":0.5,"y":0.5,"visible":"true"}},
            {"id":12,"label":"valid","type":"circle","center":{"x":0,"y":0,"visible":true}}
        ]}""")).forms
        assertEquals(listOf(12), forms.map { it.id })
        assertTrue(forms.single().center!!.visible)
    }

    @Test fun duplicateNativeIdsAreAmbiguousAndCannotBeSelected() {
        val points = requireNotNull(parseMaskPhotoPoints("""{"forms":[
            {"id":2,"label":"first","type":"circle","center":{"x":0.1,"y":0.2,"visible":true}},
            {"id":2,"label":"second","type":"circle","center":{"x":0.3,"y":0.4,"visible":true}},
            {"id":3,"label":"unique","type":"path","source":{"x":null,"y":null,"visible":false}}
        ]}"""))
        assertEquals(listOf(3), points.forms.map { it.id })
        assertNotNull(points.forms.single().source)
        assertFalse(points.forms.single().source!!.visible)
    }
}
