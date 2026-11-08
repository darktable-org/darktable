/* SPDX-License-Identifier: GPL-3.0-or-later */
package org.example.darktableandroid

import android.content.ContentResolver
import android.content.Intent
import android.net.Uri
import androidx.core.content.IntentCompat

/** Route a single document into the same content-URI cache used by the picker. */
internal fun sourceUriFromIntent(intent: Intent): Uri? {
    fun Uri?.document(): Uri? = takeIf { it?.scheme == ContentResolver.SCHEME_CONTENT }
    return when(intent.action) {
        Intent.ACTION_VIEW, Intent.ACTION_EDIT -> intent.data.document()
        Intent.ACTION_SEND -> {
            val stream = try {
                IntentCompat.getParcelableExtra(intent, Intent.EXTRA_STREAM, Uri::class.java)
            } catch(_: RuntimeException) {
                null
            }
            stream.document()?.let { return it }
            val clip = try { intent.clipData } catch(_: RuntimeException) { null }
            if(clip != null) {
                // The editor opens one photo; never silently choose a first file.
                if(clip.itemCount != 1) return null
                clip.getItemAt(0).uri.document()?.let { return it }
            }
            intent.data.document()
        }
        else -> null
    }
}
