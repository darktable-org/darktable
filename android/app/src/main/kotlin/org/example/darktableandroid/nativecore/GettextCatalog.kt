/* SPDX-License-Identifier: GPL-3.0-or-later */
package org.example.darktableandroid.nativecore

import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.nio.charset.CodingErrorAction
import java.util.Locale

/** Read-only UTF-8 GNU gettext catalog. This never changes the process locale. */
internal class GettextCatalog private constructor(
    private val messages: Map<String, String>,
    private val contexts: Map<String, Map<String, String>>,
    private val uniqueContexts: Map<String, String>,
    private val overrides: Map<String, String> = emptyMap(),
) {
    fun withOverrides(values: Map<String, String>): GettextCatalog = GettextCatalog(
        messages, contexts, uniqueContexts,
        overrides + values.filter { (source, translated) -> source.isNotEmpty() && translated.isNotEmpty() })

    fun translate(message: String, context: String? = null): String = overrides[message]
        ?: context?.let { contexts[message]?.get(it) }
        ?: messages[message]
        ?: uniqueContexts[message]
        ?: message

    companion object {
        private const val MAX_BYTES = 16 * 1024 * 1024
        private const val MAX_MESSAGES = 100_000
        private val identity = GettextCatalog(emptyMap(), emptyMap(), emptyMap())

        fun empty(): GettextCatalog = identity

        /** Invalid or unsupported catalogs fail closed so the caller can retain English. */
        fun parse(bytes: ByteArray): GettextCatalog? = try {
            require(bytes.size in 28..MAX_BYTES)
            val buffer = ByteBuffer.wrap(bytes).order(ByteOrder.LITTLE_ENDIAN)
            when(buffer.getInt(0)) {
                0x950412de.toInt() -> Unit
                0xde120495.toInt() -> buffer.order(ByteOrder.BIG_ENDIAN)
                else -> throw IllegalArgumentException("Invalid gettext magic")
            }
            fun uint(offset: Int) = buffer.getInt(offset).toLong() and 0xffffffffL
            require(uint(4) == 0L) // Standard revision; no system-dependent string extensions.
            val count = uint(8).also { require(it <= MAX_MESSAGES) }.toInt()
            fun table(offset: Int): Int = uint(offset).also {
                require(it >= 28 && it + count.toLong() * 8 <= bytes.size)
            }.toInt()
            val originals = table(12)
            val translations = table(16)
            if(count > 0) require(originals.toLong() + count * 8L <= translations ||
                translations.toLong() + count * 8L <= originals)

            val decoder = Charsets.UTF_8.newDecoder().onMalformedInput(CodingErrorAction.REPORT)
                .onUnmappableCharacter(CodingErrorAction.REPORT)
            var decodedBytes = 0L
            fun string(table: Int, index: Int): String {
                val length = uint(table + index * 8)
                val start = uint(table + index * 8 + 4)
                require(start >= 28 && start + length < bytes.size && bytes[(start + length).toInt()] == 0.toByte())
                decodedBytes += length
                require(decodedBytes <= MAX_BYTES * 2L)
                return decoder.decode(ByteBuffer.wrap(bytes, start.toInt(), length.toInt())).toString()
            }

            val messages = mutableMapOf<String, String>()
            val contextual = mutableMapOf<String, MutableMap<String, String>>()
            for(index in 0 until count) {
                val original = string(originals, index).substringBefore('\u0000')
                val translated = string(translations, index).substringBefore('\u0000')
                if(original.isEmpty()) {
                    val charset = Regex("charset\\s*=\\s*([^\\s;]+)", RegexOption.IGNORE_CASE)
                        .find(translated)?.groupValues?.get(1)
                    require(charset == null || charset.equals("UTF-8", true) || charset.equals("UTF8", true))
                    continue
                }
                if(translated.isEmpty()) continue
                val separator = original.indexOf('\u0004')
                val target = if(separator >= 0) contextual.getOrPut(original.substring(separator + 1)) { mutableMapOf() }
                    else messages
                val key = if(separator >= 0) original.substring(0, separator) else original
                val previous = target.put(key, translated)
                require(previous == null || previous == translated)
            }
            GettextCatalog(messages.toMap(), contextual.mapValues { it.value.toMap() },
                contextual.mapNotNull { (message, values) ->
                    values.values.distinct().singleOrNull()?.let { message to it }
                }.toMap())
        } catch(_: Exception) {
            null
        }
    }
}

/** Catalog names are fixed assets, never paths supplied by a locale string. */
internal fun catalogLocaleTag(languageTag: String): String? {
    val locale = Locale.forLanguageTag(languageTag.replace('_', '-'))
    return when(locale.language) {
        "zh" -> when {
            locale.script.equals("Hant", true) -> "zh_TW"
            locale.script.equals("Hans", true) -> "zh_CN"
            locale.country in setOf("TW", "HK", "MO") -> "zh_TW"
            else -> "zh_CN"
        }
        "es" -> "es"
        else -> null
    }
}
