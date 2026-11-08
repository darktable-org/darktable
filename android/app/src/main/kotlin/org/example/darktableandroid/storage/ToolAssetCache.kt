/* SPDX-License-Identifier: GPL-3.0-or-later */
package org.example.darktableandroid.storage

import android.content.ContentResolver
import android.content.Context
import android.net.Uri
import android.provider.OpenableColumns
import java.io.File
import java.io.InputStream
import java.security.MessageDigest

/** A session owns these immutable files until its native history is closed. */
internal class ToolAssetCache(private val context: Context) {
    fun copyForNative(uri: Uri, directory: File, extensions: List<String>, isCancelled: () -> Boolean): File {
        require(uri.scheme == ContentResolver.SCHEME_CONTENT) { "Choose a file from the document picker" }
        val name = context.contentResolver.query(uri, arrayOf(OpenableColumns.DISPLAY_NAME), null, null, null)
            ?.use { cursor -> if(cursor.moveToFirst()) cursor.getString(0) else null }
        val extension = safeExtension(name)
        require(extension in extensions) { "Choose a ${extensions.joinToString(" / ")} file" }
        return context.contentResolver.openInputStream(uri).use { input ->
            checkNotNull(input) { "The document provider returned no input stream" }
            cacheToolAsset(input, extension, directory, isCancelled = isCancelled)
        }
    }
}

/** No pruning: an older file may still be referenced by an undo snapshot. */
internal fun cacheToolAsset(
    input: InputStream,
    extension: String,
    directory: File,
    maxBytes: Long = 512L * 1024 * 1024,
    isCancelled: () -> Boolean = { false },
): File {
    require(extension.length in 1..10 && extension.all { it in 'a'..'z' || it in '0'..'9' })
    check(directory.isDirectory || directory.mkdirs()) { "Unable to create the tool file cache" }
    val incoming = File.createTempFile("incoming-", ".$extension.part", directory)
    val digest = MessageDigest.getInstance("SHA-256")
    try {
        var total = 0L
        incoming.outputStream().buffered().use { output ->
            val buffer = ByteArray(DEFAULT_BUFFER_SIZE)
            while(true) {
                check(!isCancelled()) { "File import cancelled" }
                val count = input.read(buffer)
                if(count < 0) break
                total += count
                check(total <= maxBytes) { "The tool file exceeds the session cache limit" }
                digest.update(buffer, 0, count)
                output.write(buffer, 0, count)
            }
        }
        check(total > 0) { "The selected file is empty" }
        check(!isCancelled()) { "File import cancelled" }
        val hash = digest.digest().joinToString("") { "%02x".format(it) }
        val stable = directory.resolve("$hash.$extension")
        if(!stable.exists()) {
            val retainedBytes = directory.listFiles().orEmpty().filter { it != incoming }.sumOf { it.length() }
            check(retainedBytes <= maxBytes - total) { "The image session's tool file cache is full" }
            check(incoming.renameTo(stable)) { "Unable to finalize the imported tool file" }
        }
        return stable
    } finally {
        incoming.delete()
    }
}
