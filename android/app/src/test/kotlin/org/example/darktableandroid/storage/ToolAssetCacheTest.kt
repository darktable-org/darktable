/* SPDX-License-Identifier: GPL-3.0-or-later */
package org.example.darktableandroid.storage

import java.io.ByteArrayInputStream
import java.nio.file.Files
import org.junit.Assert.*
import org.junit.Test

class ToolAssetCacheTest {
    @Test fun filesUsedByEarlierHistoryRemainImmutableAndIdenticalImportsReuseThem() {
        val directory = Files.createTempDirectory("tool-assets").toFile()
        try {
            val first = cacheToolAsset(ByteArrayInputStream(byteArrayOf(1, 2, 3)), "cube", directory, maxBytes = 6)
            val second = cacheToolAsset(ByteArrayInputStream(byteArrayOf(4, 5, 6)), "cube", directory, maxBytes = 6)
            val duplicate = cacheToolAsset(ByteArrayInputStream(byteArrayOf(1, 2, 3)), "cube", directory, maxBytes = 6)
            assertEquals(first, duplicate)
            assertNotEquals(first, second)
            assertArrayEquals(byteArrayOf(1, 2, 3), first.readBytes())
            assertArrayEquals(byteArrayOf(4, 5, 6), second.readBytes())
            assertEquals(2, directory.listFiles()!!.size)
            assertThrows(IllegalStateException::class.java) {
                cacheToolAsset(ByteArrayInputStream(byteArrayOf(7)), "cube", directory, maxBytes = 6)
            }
            assertEquals(2, directory.listFiles()!!.size)
            assertTrue(first.exists())
        } finally { directory.deleteRecursively() }
    }

    @Test fun cancelledAndOversizedCopiesRemoveOnlyTheirPartialFile() {
        val directory = Files.createTempDirectory("tool-assets").toFile()
        try {
            val retained = cacheToolAsset(ByteArrayInputStream(byteArrayOf(1)), "png", directory, maxBytes = 4)
            assertThrows(IllegalStateException::class.java) {
                cacheToolAsset(ByteArrayInputStream(byteArrayOf(2, 3)), "png", directory, maxBytes = 4) { true }
            }
            assertThrows(IllegalStateException::class.java) {
                cacheToolAsset(ByteArrayInputStream(ByteArray(5)), "png", directory, maxBytes = 4)
            }
            assertArrayEquals(byteArrayOf(1), retained.readBytes())
            assertEquals(listOf(retained), directory.listFiles()!!.toList())
        } finally { directory.deleteRecursively() }
    }
}
