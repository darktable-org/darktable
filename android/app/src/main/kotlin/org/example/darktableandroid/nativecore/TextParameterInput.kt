/* SPDX-License-Identifier: GPL-3.0-or-later */
package org.example.darktableandroid.nativecore

import java.nio.CharBuffer
import java.nio.charset.CharacterCodingException
import java.nio.charset.CodingErrorAction

/** Native buffers are bounded in UTF-8 bytes; character counts alone are insufficient. */
internal fun ModuleTextParameter.accepts(text: String): Boolean {
    if(maxBytes !in 1..65535 || '\u0000' in text) return false
    // The native watermark template setting selects a resource basename.
    if(name == "filename" && (text.isEmpty() || text == "." || text == ".." || '/' in text || '\\' in text)) return false
    return try {
        Charsets.UTF_8.newEncoder()
            .onMalformedInput(CodingErrorAction.REPORT)
            .onUnmappableCharacter(CodingErrorAction.REPORT)
            .encode(CharBuffer.wrap(text)).remaining() <= maxBytes
    } catch(_: CharacterCodingException) {
        false
    }
}

/** Profile picks use a strict native action, with the original string buffer's
 * byte limit. Their writable identities are advertised by the lens capability
 * object even on runtimes which retain legacy camera/lens text metadata. */
internal fun EditorModule.textParameterForWrite(name: String, value: String): ModuleTextParameter? {
    val legacy = when(name) {
        "lens.camera_profile" -> "camera"
        "lens.profile" -> "lens"
        else -> return textParameters.singleOrNull { it.name == name }?.takeIf { it.accepts(value) }
    }
    if(operation.substringBefore('@') != "lens") return null
    val info = lensCorrection?.takeIf { it.method == 1 && it.databaseAvailable } ?: return null
    val choices = if(legacy == "camera") info.cameras else info.lenses
    if(choices.none { it.value == value }) return null
    return (textParameters.singleOrNull { it.name == name }
        ?: textParameters.singleOrNull { it.name == legacy }?.copy(name = name))?.takeIf { it.accepts(value) }
}
