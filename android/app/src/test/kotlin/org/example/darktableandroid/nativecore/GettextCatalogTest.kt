/* SPDX-License-Identifier: GPL-3.0-or-later */
package org.example.darktableandroid.nativecore

import java.nio.ByteBuffer
import java.nio.ByteOrder
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertSame
import org.junit.Test

class GettextCatalogTest {
    private fun mo(entries: List<Pair<String, String>>, bigEndian: Boolean = false): ByteArray {
        val originals = entries.map { it.first.toByteArray(Charsets.UTF_8) }
        val translated = entries.map { it.second.toByteArray(Charsets.UTF_8) }
        val size = 28 + entries.size * 16 + originals.sumOf { it.size + 1 } + translated.sumOf { it.size + 1 }
        val bytes = ByteArray(size)
        val buffer = ByteBuffer.wrap(bytes).order(if(bigEndian) ByteOrder.BIG_ENDIAN else ByteOrder.LITTLE_ENDIAN)
        val sourceTable = 28
        val targetTable = sourceTable + entries.size * 8
        buffer.putInt(0, 0x950412de.toInt()).putInt(4, 0).putInt(8, entries.size)
            .putInt(12, sourceTable).putInt(16, targetTable).putInt(20, 0).putInt(24, 0)
        var cursor = 28 + entries.size * 16
        listOf(sourceTable to originals, targetTable to translated).forEach { (table, strings) ->
            strings.forEachIndexed { index, value ->
                buffer.putInt(table + index * 8, value.size).putInt(table + index * 8 + 4, cursor)
                value.copyInto(bytes, cursor)
                cursor += value.size + 1
            }
        }
        return bytes
    }

    private fun catalog(vararg entries: Pair<String, String>) = requireNotNull(GettextCatalog.parse(mo(entries.toList())))

    @Test fun readsBothByteOrdersAndRetainsUnknownLabels() {
        listOf(false, true).forEach { bigEndian ->
            val catalog = requireNotNull(GettextCatalog.parse(mo(listOf(
                "" to "Content-Type: text/plain; charset=UTF-8\n",
                "exposure" to "曝光", "orientation" to "orientación", "manual" to "手動",
            ), bigEndian)))
            assertEquals("曝光", catalog.translate("exposure"))
            assertEquals("orientación", catalog.translate("orientation"))
            assertEquals("unknown.module.id", catalog.translate("unknown.module.id"))
        }
        assertEquals("exposure", GettextCatalog.empty().translate("exposure"))
    }

    @Test fun contextsUseExactKnownOrUnambiguousTranslationsOnly() {
        val catalog = catalog("manual" to "手動", "tool\u0004manual" to "專用手動",
            "white balance\u0004as shot" to "拍攝時設定", "other\u0004as shot" to "另一含義",
            "white balance\u0004camera reference" to "相機參考")
        assertEquals("專用手動", catalog.translate("manual", "tool"))
        assertEquals("手動", catalog.translate("manual"))
        assertEquals("手動", catalog.translate("manual", "unknown context"))
        assertEquals("手機手動", catalog.withOverrides(mapOf("manual" to "手機手動")).translate("manual", "tool"))
        assertEquals("拍攝時設定", catalog.translate("as shot", "white balance"))
        assertEquals("as shot", catalog.translate("as shot"))
        assertEquals("相機參考", catalog.translate("camera reference"))
    }

    @Test fun singularLabelsReadTheFirstPluralEntryAndEmptyTranslationsFallBack() {
        val catalog = catalog("image\u0000images" to "imagen\u0000imágenes", "red" to "")
        assertEquals("imagen", catalog.translate("image"))
        assertEquals("red", catalog.translate("red"))
    }

    @Test fun malformedCatalogsNeverExposePartialTranslations() {
        val original = mo(listOf("exposure" to "曝光"))
        val invalid = listOf(ByteArray(0), ByteArray(28), original.copyOf(27),
            original.copyOf().apply { ByteBuffer.wrap(this).order(ByteOrder.LITTLE_ENDIAN).putInt(4, 1) },
            original.copyOf().apply { ByteBuffer.wrap(this).order(ByteOrder.LITTLE_ENDIAN).putInt(8, -1) },
            original.copyOf().apply { ByteBuffer.wrap(this).order(ByteOrder.LITTLE_ENDIAN).putInt(12, -1) },
            original.copyOf().apply { ByteBuffer.wrap(this).order(ByteOrder.LITTLE_ENDIAN).putInt(32, -1) },
            original.copyOf().apply { this[lastIndex] = 1 },
            original.copyOf().apply { this[44] = 0xff.toByte() },
            mo(listOf("" to "Content-Type: text/plain; charset=ISO-8859-1\n", "red" to "rojo")))
        invalid.forEach { assertNull(GettextCatalog.parse(it)) }
        assertNotNull(GettextCatalog.parse(original))
    }

    @Test fun localeMappingHonorsChineseScriptBeforeRegionAndUsesSpanishFallback() {
        listOf("zh-Hant", "zh-TW", "zh-HK", "zh-MO", "zh_TW").forEach { assertEquals("zh_TW", catalogLocaleTag(it)) }
        listOf("zh", "zh-CN", "zh-SG", "zh-Hans-HK").forEach { assertEquals("zh_CN", catalogLocaleTag(it)) }
        listOf("es-419", "es-MX", "es-AR", "es").forEach { assertEquals("es", catalogLocaleTag(it)) }
        listOf("en", "en-US", "fr", "../es").forEach { assertNull(catalogLocaleTag(it)) }
    }

    @Test fun mobileOverridesAreImmutableAndTranslateCompoundLabelsWithoutChangingIndexes() {
        val original = catalog("red" to "rojo", "blue" to "azul", "channel" to "canal", "band" to "banda")
        val catalog = original.withOverrides(mapOf("red" to "rojo móvil", "band strength" to "intensidad",
            "hue" to "matiz", "%s output" to "Salida de %1\$s", "" to "invalid", "empty" to ""))
        assertEquals("rojo", original.translate("red"))
        assertEquals("rojo móvil", catalog.translate("red"))
        assertEquals("empty", catalog.translate("empty"))
        assertEquals("rojo móvil / azul", catalog.translateLabel("red / blue"))
        assertEquals("intensidad · canal 2 · banda 7", catalog.translateLabel("band strength · channel 2 · band 7"))
        assertEquals("Salida de matiz", catalog.translateLabel("hue output"))
        assertEquals("filename.raw", catalog.translateLabel("filename.raw"))
        val module = EditorModule("exposure@2", "red · blue / channel 7", true, true, emptyList())
        assertEquals("rojo móvil · blue / channel 7", module.localized(catalog).name)
    }

    @Test fun localizesOnlyDisplayFieldsAndPreservesAllNativeBindingsAndUserNames() {
        val catalog = catalog("exposure" to "曝光", "manual" to "手動", "red" to "紅色",
            "as shot" to "拍攝時設定", "daylight" to "日光", "image format" to "影像格式")
        val balance = WhiteBalance(listOf(WhiteBalanceChannel(0, "red", 1.5f)), .1f, 8f,
            0, "as shot", listOf(WhiteBalanceSetting(0, "as shot")),
            cameraPresets = WhiteBalanceCameraPresets("red", listOf(WhiteBalancePreset(6, "daylight", listOf(-1, 0, 1), 0)), 6, 1))
        val lens = LensCorrection("ready", "manual", 1, false, true, true, true, 4,
            "red", "manual", listOf(LensProfileChoice("camera-id", "exposure")),
            listOf(LensProfileChoice("lens-id", "red")))
        val module = EditorModule("exposure@7", "exposure · manual", true, false,
            listOf(ModuleParameter("manual", "exposure", "enum", 2.0, -4.0, 5.0,
                listOf(ParameterChoice(2.0, "manual")), -1.0, 3.0, 1.0)),
            curves = listOf(ModuleCurve(2, "red", true, 1, 20, .001f, emptyList(), emptyList())),
            whiteBalance = balance, textParameters = listOf(ModuleTextParameter("red", "manual", "exposure", 1024,
                defaultValue = "manual", isFile = true, extensions = listOf("red"))), lensCorrection = lens)
        val translated = module.localized(catalog)
        assertEquals("曝光 · manual", translated.name)
        assertEquals("exposure@7", translated.operation)
        assertEquals(module.parameters.single().copy(label = "曝光", choices = listOf(ParameterChoice(2.0, "手動"))),
            translated.parameters.single())
        assertEquals(module.textParameters.single().copy(label = "手動"), translated.textParameters.single())
        assertEquals(module.curves.single().copy(label = "紅色"), translated.curves.single())
        assertEquals("紅色", translated.whiteBalance!!.channels.single().label)
        assertEquals("拍攝時設定", translated.whiteBalance!!.settingLabel)
        assertEquals("red", translated.whiteBalance!!.cameraPresets!!.camera)
        assertEquals("日光", translated.whiteBalance!!.cameraPresets!!.presets.single().label)
        assertEquals(balance.cameraPresets!!.presets.single().tuning,
            translated.whiteBalance!!.cameraPresets!!.presets.single().tuning)
        assertSame(lens, translated.lensCorrection)
        assertEquals("exposure", module.name.substringBefore(" · "))
        assertEquals("manual", module.parameters.single().choices.single().label)
    }

    @Test fun exportNamesAndChoicesTranslateWithoutChangingProfileNamesOrEncodingCapabilities() {
        val catalog = catalog("manual" to "手動", "red" to "紅色")
        val size = ExportNumberOption(0, 1, 4096, 1600)
        val options = ExportOptions(size, size, listOf(ExportOptionChoice(1, "manual")), 1,
            listOf(ExportOptionChoice(0, "red")), 0, listOf(0, 2), 2,
            listOf(ExportOptionChoice(0, "manual")), 0)
        val format = ExportFormat("png", "manual", "png", "image/png", 16, options)
        val translated = format.localized(catalog)
        assertEquals(format.copy(name = "手動", options = options.copy(
            pixelFormats = listOf(ExportOptionChoice(1, "手動")),
            metadataPolicies = listOf(ExportOptionChoice(0, "手動")))), translated)
        assertSame(options.profiles, translated.options!!.profiles)
        assertEquals("manual", format.name)
    }
}
