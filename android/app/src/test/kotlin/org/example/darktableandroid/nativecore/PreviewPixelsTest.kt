/* SPDX-License-Identifier: GPL-3.0-or-later */
package org.example.darktableandroid.nativecore

import org.junit.Assert.assertArrayEquals
import org.junit.Test

class PreviewPixelsTest {
    @Test fun desktopPreviewPreservesRedGreenBlueAndAlpha() {
        val bytes = byteArrayOf(
            0, 0, 0xff.toByte(), 0xff.toByte(),
            0, 0xff.toByte(), 0, 0xff.toByte(),
            0xff.toByte(), 0, 0, 0xff.toByte(),
            0x33, 0x22, 0x11, 0x80.toByte(),
        )
        assertArrayEquals(intArrayOf(0xffff0000.toInt(), 0xff00ff00.toInt(), 0xff0000ff.toInt(), 0x80112233.toInt()),
            previewArgbPixels(bytes))
    }
}
