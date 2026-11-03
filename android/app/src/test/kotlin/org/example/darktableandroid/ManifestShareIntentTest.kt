/* SPDX-License-Identifier: GPL-3.0-or-later */
package org.example.darktableandroid

import android.content.Intent
import android.content.pm.PackageManager
import android.net.Uri
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.RuntimeEnvironment
import org.robolectric.annotation.Config
import org.robolectric.annotation.GraphicsMode

/** Resolves the merged app manifest, without registering a synthetic Activity or launching native code. */
@RunWith(RobolectricTestRunner::class)
@Config(sdk = [35])
@GraphicsMode(GraphicsMode.Mode.NATIVE)
class ManifestShareIntentTest {
    private val source = Uri.parse("content://files.example/document/3819")

    @Suppress("DEPRECATION")
    private fun offersEditor(intent: Intent): Boolean {
        val app = RuntimeEnvironment.getApplication()
        intent.setPackage(app.packageName)
        return app.packageManager.queryIntentActivities(intent, PackageManager.MATCH_DEFAULT_ONLY).any {
            it.activityInfo.packageName == app.packageName && it.activityInfo.name == MainActivity::class.java.name
        }
    }

    private fun share(type: String) = Intent(Intent.ACTION_SEND).apply {
        this.type = type
        putExtra(Intent.EXTRA_STREAM, source)
        addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION)
    }

    @Test fun gprAndImageSharesDiscoverTheRealEditorActivity() {
        listOf("image/x-gopro-gpr", "image/gpr", "image/x-adobe-dng", "image/jpeg", "image/*").forEach { type ->
            assertTrue("The editor is absent for ACTION_SEND $type", offersEditor(share(type)))
        }
    }

    @Test fun genericBinaryAndWildcardSharesWorkWithOpaqueDocumentUris() {
        // A file manager's share URI need not contain the displayed .GPR name.
        listOf("application/octet-stream", "application/x-gopro-gpr", "*/*").forEach { type ->
            assertTrue("The editor is absent for ACTION_SEND $type", offersEditor(share(type)))
        }
    }

    @Test fun existingRawViewAndEditIntentFiltersRemainAvailable() {
        listOf(Intent.ACTION_VIEW, Intent.ACTION_EDIT).forEach { action ->
            assertTrue("The existing $action entry point disappeared",
                offersEditor(Intent(action).setDataAndType(source, "image/x-gopro-gpr")))
        }
    }

    @Test fun unrelatedDocumentsAndMultiPhotoSharesAreNotAdvertised() {
        listOf("text/plain", "application/pdf").forEach { type ->
            assertFalse("An unrelated $type document matched", offersEditor(share(type)))
        }
        assertFalse(offersEditor(Intent(Intent.ACTION_SEND_MULTIPLE).apply {
            type = "image/x-gopro-gpr"
            putParcelableArrayListExtra(Intent.EXTRA_STREAM, arrayListOf(source))
        }))
    }
}
