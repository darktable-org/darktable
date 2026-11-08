/* SPDX-License-Identifier: GPL-3.0-or-later */
package org.example.darktableandroid.nativecore

import java.nio.ByteBuffer
import java.nio.ByteOrder

/** The desktop preview buffer contains B,G,R,A bytes. Bitmap color ints use 0xAARRGGBB. */
internal fun previewArgbPixels(bgra: ByteArray): IntArray {
    require(bgra.size % 4 == 0) { "The preview buffer contains an incomplete pixel" }
    return IntArray(bgra.size / 4).also { pixels ->
        ByteBuffer.wrap(bgra).order(ByteOrder.LITTLE_ENDIAN).asIntBuffer().get(pixels)
    }
}
