/* SPDX-License-Identifier: GPL-3.0-or-later */
package org.example.darktableandroid

import android.content.ClipData
import android.content.Intent
import android.net.Uri
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.annotation.Config
import org.robolectric.annotation.GraphicsMode

/** Intent parsing only: these fixtures do not decode a RAW file or launch the engine. */
@RunWith(RobolectricTestRunner::class)
@Config(sdk = [35])
@GraphicsMode(GraphicsMode.Mode.NATIVE)
class IncomingPhotoIntentTest {
    private val source = Uri.parse("content://documents.example/document/1782")
    private val other = Uri.parse("content://documents.example/document/9301")

    @Test fun viewAndEditKeepTheirExistingDataUriRoute() {
        listOf(Intent.ACTION_VIEW, Intent.ACTION_EDIT).forEach { action ->
            assertEquals(source, sourceUriFromIntent(Intent(action).setDataAndType(source, "image/x-gopro-gpr")))
        }
    }

    @Test fun sharedStreamTakesPrecedenceWithoutChangingThePermissionGrant() {
        val flags = Intent.FLAG_GRANT_READ_URI_PERMISSION or Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION
        val intent = Intent(Intent.ACTION_SEND).apply {
            type = "application/octet-stream"
            data = other
            putExtra(Intent.EXTRA_STREAM, source)
            clipData = ClipData.newRawUri("photo", other)
            addFlags(flags)
        }

        assertEquals(source, sourceUriFromIntent(intent))
        assertEquals(flags, intent.flags)
        assertEquals(other, intent.data)
        assertEquals(other, intent.clipData!!.getItemAt(0).uri)
    }

    @Test fun clipDataOnlySharesOpenTheirSingleDocument() {
        val intent = Intent(Intent.ACTION_SEND).apply {
            type = "image/x-gopro-gpr"
            clipData = ClipData.newRawUri("GPR", source)
        }
        assertEquals(source, sourceUriFromIntent(intent))
    }

    @Test fun dataOnlyShareSendersKeepTheirCompatibilityRoute() {
        assertEquals(source, sourceUriFromIntent(Intent(Intent.ACTION_SEND).setDataAndType(source, "image/gpr")))
    }

    @Test fun malformedStreamExtraCanFallBackToAValidClipDocument() {
        val intent = Intent(Intent.ACTION_SEND).apply {
            type = "application/octet-stream"
            putExtra(Intent.EXTRA_STREAM, "not a parcelled URI")
            clipData = ClipData.newRawUri("GPR", source)
        }
        assertEquals(source, sourceUriFromIntent(intent))
    }

    @Test fun unsupportedActionsSchemesAndMissingDocumentsDoNotOpenAPhoto() {
        val rejected = listOf(
            Intent(Intent.ACTION_MAIN).setData(source),
            Intent(Intent.ACTION_SEND_MULTIPLE).putParcelableArrayListExtra(Intent.EXTRA_STREAM, arrayListOf(source, other)),
            Intent(Intent.ACTION_VIEW),
            Intent(Intent.ACTION_VIEW, Uri.parse("https://example.test/image.gpr")),
            Intent(Intent.ACTION_SEND).putExtra(Intent.EXTRA_TEXT, "a photo description"),
            Intent(Intent.ACTION_SEND).putExtra(Intent.EXTRA_STREAM, "not a parcelled URI"),
            Intent(Intent.ACTION_SEND).putExtra(Intent.EXTRA_STREAM, Uri.parse("https://example.test/image.gpr")),
        )
        rejected.forEach { intent -> assertNull("Unexpected source for ${intent.action}", sourceUriFromIntent(intent)) }
    }

    @Test fun multipleClipDocumentsAreNotSilentlyReducedToTheFirstPhoto() {
        val intent = Intent(Intent.ACTION_SEND).apply {
            type = "application/octet-stream"
            clipData = ClipData.newRawUri("GPR", source).apply { addItem(ClipData.Item(other)) }
        }
        assertNull(sourceUriFromIntent(intent))
    }

    @Test fun fileUrisDoNotBypassTheContentOnlyImportPath() {
        val file = Uri.parse("file:///storage/emulated/0/DCIM/GOPR0123.GPR")
        assertNull(sourceUriFromIntent(Intent(Intent.ACTION_SEND).putExtra(Intent.EXTRA_STREAM, file)))
    }
}
