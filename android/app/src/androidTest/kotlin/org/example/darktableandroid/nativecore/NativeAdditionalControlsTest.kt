/* SPDX-License-Identifier: GPL-3.0-or-later */
package org.example.darktableandroid.nativecore

import android.graphics.Bitmap
import android.graphics.Color
import android.os.Build
import android.os.SystemClock
import android.util.AtomicFile
import android.util.Log
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.filters.LargeTest
import androidx.test.platform.app.InstrumentationRegistry
import java.io.File
import java.io.IOException
import java.security.MessageDigest
import java.util.UUID
import org.json.JSONArray
import org.json.JSONObject
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Assume.assumeTrue
import org.junit.Test
import org.junit.runner.RunWith

/** Exercises reviewed desktop controls through real JNI, independently of the
 * Compose interaction tests. The generated PNG is a lossless input fixture;
 * native darktable modules perform every adjustment and pixel conversion. */
@RunWith(AndroidJUnit4::class)
@LargeTest
class NativeAdditionalControlsTest {
    private val context get() = InstrumentationRegistry.getInstrumentation().targetContext
    private val report = JSONObject()
    private val phases = JSONArray()
    private val edits = JSONArray()
    private val started = SystemClock.elapsedRealtime()
    private lateinit var resultFile: AtomicFile
    private var previewCount = 0

    @Test
    fun originalAdditionalControlsTypesPixelsAndHistory() {
        val required = InstrumentationRegistry.getArguments().getString("requireAdditionalControls") == "true"
        val directory = File(context.filesDir, "additional-controls").apply { check(mkdirs() || isDirectory) }
        resultFile = AtomicFile(File(directory, "result.json"))
        report.put("status", "running").put("phase", "prepare-fixture").put("required", required)
            .put("phases", phases).put("edits", edits).put("packageName", context.packageName)
            .put("device", JSONObject().put("sdk", Build.VERSION.SDK_INT).put("fingerprint", Build.FINGERPRINT))
        save()
        var handle = 0L
        try {
            val input = phase("create-small-rgb-png-fixture") {
                val run = File(directory, "run-${UUID.randomUUID()}").apply { check(mkdir()) }
                val file = File(run, "source.png")
                val bitmap = Bitmap.createBitmap(32, 24, Bitmap.Config.ARGB_8888)
                try {
                    for(y in 0 until 24) for(x in 0 until 32) {
                        bitmap.setPixel(x, y, Color.rgb(20 + (x * 5 + y * 3) % 200,
                            20 + (x * 3 + y * 7) % 200, 20 + (x * 7 + y * 5) % 200))
                    }
                    file.outputStream().use { assertTrue(bitmap.compress(Bitmap.CompressFormat.PNG, 100, it)) }
                } finally { bitmap.recycle() }
                file
            }
            val before = sha256(input)
            report.put("inputFile", input.absolutePath).put("sourceSha256Before", before)
            val capabilities = phase("load-native-capabilities") { NativeCore.capabilities() }
            report.put("capabilities", capabilities)
            if(!required && capabilities and 1 == 0) {
                report.put("status", "skipped").put("reason", "Real engine absent; requireAdditionalControls=true makes this mandatory")
                save()
                assumeTrue("Requires the original native engine", false)
            }
            assertEquals("Requires the original native engine", 1, capabilities and 1)
            phase("configure-original-runtime") { RuntimeAssets.initializeNative(context) }
            phase("open-png-through-original-engine") {
                handle = NativeCore.open(input.absolutePath)
                assertTrue(handle != 0L)
            }
            phase("verify-tone-mapping-native-metadata") {
                verifyFloat(handle, "tonemap", "contrast", 1.0, 5.0)
                verifyFloat(handle, "tonemap", "Fsize", 0.0, 100.0)
                rejectEdit(handle, "tonemap", "contrast", Double.NaN)
                rejectEdit(handle, "tonemap", "contrast", Double.POSITIVE_INFINITY)
                rejectEdit(handle, "tonemap", "Fsize", -1.0)
                rejectEdit(handle, "tonemap", "Fsize", 101.0)
            }
            phase("verify-reviewed-controls-in-mobile-model") {
                val modules = parseModules(NativeCore.listModules(handle))
                assertTrue(modules.single { it.operation == "tonemap" }.hasEditableControls)
                assertEquals(setOf("contrast", "Fsize"), modules.single { it.operation == "tonemap" }
                    .parameters.map { it.name }.toSet())
            }
            phase("verify-native-enum-and-boolean-metadata") {
                verifyChoices(handle, "globaltonemap", "operator", setOf(0.0, 1.0, 2.0))
                verifyChoices(handle, "lowpass", "lowpass_algo", setOf(0.0, 1.0))
                verifyChoices(handle, "vignette", "dithering", setOf(0.0, 1.0, 2.0))
                val automatic = parameter(module(handle, "vignette"), "autoratio")
                assertEquals("bool", automatic.getString("type"))
                assertEquals(0.0, automatic.getDouble("min"), 0.0)
                assertEquals(1.0, automatic.getDouble("max"), 0.0)
                rejectEdit(handle, "globaltonemap", "operator", 3.0)
                rejectEdit(handle, "globaltonemap", "operator", 0.5)
                rejectEdit(handle, "lowpass", "lowpass_algo", -1.0)
                rejectEdit(handle, "lowpass", "lowpass_algo", 0.5)
                rejectEdit(handle, "vignette", "dithering", 3.0)
                rejectEdit(handle, "vignette", "autoratio", 0.5)
                rejectEdit(handle, "vignette", "autoratio", 2.0)
                rejectEdit(handle, "vignette", "whratio", 0.0)
                rejectEdit(handle, "vignette", "whratio", 2.0)
                rejectEdit(handle, "vignette", "center.x", -1.1)
                rejectEdit(handle, "vignette", "center.y", 1.1)
            }
            phase("verify-defaults-and-touch-ranges") {
                val modules = JSONArray(NativeCore.listModules(handle))
                var defaultCount = 0
                var sliderCount = 0
                for(moduleIndex in 0 until modules.length()) {
                    val parameters = modules.getJSONObject(moduleIndex).getJSONArray("parameters")
                    for(parameterIndex in 0 until parameters.length()) {
                        val item = parameters.getJSONObject(parameterIndex)
                        if(item.has("default")) {
                            val value = item.getDouble("default")
                            assertTrue(value.isFinite())
                            assertTrue(value >= item.getDouble("min") && value <= item.getDouble("max"))
                            if(item.getString("type") == "enum") {
                                assertTrue(choiceValues(item).contains(value))
                            }
                            defaultCount++
                        }
                        if(item.has("sliderMin") && item.has("sliderMax")) {
                            val minimum = item.getDouble("sliderMin")
                            val maximum = item.getDouble("sliderMax")
                            assertTrue(minimum.isFinite() && maximum.isFinite() && minimum < maximum)
                            assertTrue(minimum >= item.getDouble("min") && maximum <= item.getDouble("max"))
                            sliderCount++
                        }
                    }
                }
                assertTrue("Original module defaults must be available for reset controls", defaultCount >= 20)
                assertTrue("Reviewed desktop soft ranges must be available for touch sliders", sliderCount >= 10)
                val exposure = parameter(module(handle, "exposure"), "exposure")
                assertEquals(-3.0, exposure.getDouble("sliderMin"), 0.0)
                assertEquals(4.0, exposure.getDouble("sliderMax"), 0.0)
                report.put("nativeDefaultCount", defaultCount).put("nativeSliderRangeCount", sliderCount)
            }
            metadataEdit(handle, "tonemap", "Fsize", 75.0)
            for((operation, name, requested) in listOf(
                Triple("highpass", "contrast", 25.0),
                Triple("soften", "amount", 25.0),
                Triple("lowpass", "brightness", 0.5),
                Triple("graduatednd", "density", 2.0),
                Triple("monochrome", "highlights", 0.5),
                Triple("vignette", "center.x", 0.25),
                Triple("vignette", "center.y", -0.25),
                Triple("globaltonemap", "drago.bias", 0.75),
                Triple("globaltonemap", "drago.max_light", 200.0),
            )) metadataEdit(handle, operation, name, requested)
            // Every declared enum choice is sent to the original setter, with
            // history roundtrips for each value different from the baseline.
            for((operation, name) in listOf("globaltonemap" to "operator",
                "lowpass" to "lowpass_algo", "vignette" to "dithering")) {
                val item = parameter(module(handle, operation), name)
                for(value in choiceValues(item).filter { it != item.getDouble("value") }) {
                    metadataEdit(handle, operation, name, value)
                }
            }
            val automatic = parameter(module(handle, "vignette"), "autoratio").getDouble("value")
            metadataEdit(handle, "vignette", "autoratio", if(automatic == 0.0) 1.0 else 0.0)
            defaultReset(handle, "tonemap", "Fsize", 75.0)

            phase("enable-tonemap") { NativeCore.setModuleEnabled(handle, "tonemap", true) }
            // The earlier PNG fixture exposed a first-preview initialization
            // difference. Prime explicitly and require two identical baselines;
            // no tolerance is introduced for edit/undo/redo comparisons.
            phase("prime-original-png-preview") { preview(handle) }
            val toneBaseline = phase("preview-tonemap-baseline") { preview(handle) }
            assertTrue("Unedited baseline must be stable", toneBaseline.contentEquals(
                phase("verify-tonemap-baseline-stability") { preview(handle) }))
            pixelEdit(handle, "tonemap", "contrast", 4.0, toneBaseline)
            phase("disable-tonemap") { NativeCore.setModuleEnabled(handle, "tonemap", false) }
            phase("enable-vignette") { NativeCore.setModuleEnabled(handle, "vignette", true) }
            assertEquals("Use deterministic vignette processing for exact pixel comparisons", 0.0,
                parameter(module(handle, "vignette"), "dithering").getDouble("value"), 0.0)
            val vignetteBaseline = phase("preview-vignette-baseline") { preview(handle) }
            assertTrue("Unedited vignette baseline must be stable", vignetteBaseline.contentEquals(
                phase("verify-vignette-baseline-stability") { preview(handle) }))
            pixelEdit(handle, "vignette", "brightness", -0.75, vignetteBaseline)

            phase("source-hash-after-additional-edits") {
                report.put("sourceSha256After", sha256(input))
                assertEquals(before, sha256(input))
            }
            phase("close-native-additional-session") { NativeCore.close(handle); handle = 0L }
            phase("source-hash-after-close") {
                report.put("sourceSha256AfterFinalClose", sha256(input))
                assertEquals(before, sha256(input))
            }
            report.put("status", "passed").put("phase", "complete")
                .put("previewCount", previewCount).put("previewRgbBytes", 32 * 24 * 3)
            save()
        } catch(failure: org.junit.AssumptionViolatedException) {
            throw failure
        } catch(failure: Throwable) {
            report.put("status", "failed").put("failure", Log.getStackTraceString(failure))
            save()
            throw failure
        } finally {
            if(handle != 0L) {
                try { NativeCore.close(handle) }
                catch(failure: Throwable) { report.put("closeFailure", Log.getStackTraceString(failure)); save() }
            }
        }
    }

    private fun verifyFloat(handle: Long, operation: String, name: String, minimum: Double, maximum: Double) {
        val item = parameter(module(handle, operation), name)
        assertEquals("float", item.getString("type"))
        assertEquals(minimum.toFloat().toDouble(), item.getDouble("min"), 1e-7)
        assertEquals(maximum.toFloat().toDouble(), item.getDouble("max"), 1e-7)
        assertTrue(item.getDouble("value").isFinite())
    }

    private fun verifyChoices(handle: Long, operation: String, name: String, expected: Set<Double>) {
        val item = parameter(module(handle, operation), name)
        assertEquals("enum", item.getString("type"))
        assertEquals(expected, choiceValues(item))
        val choices = item.getJSONArray("choices")
        assertEquals(expected.size, choices.length())
        for(index in 0 until choices.length()) assertTrue(choices.getJSONObject(index).getString("label").isNotBlank())
    }

    private fun choiceValues(item: JSONObject): Set<Double> = item.getJSONArray("choices").let { choices ->
        (0 until choices.length()).map { choices.getJSONObject(it).getDouble("value") }.toSet()
    }

    private fun defaultReset(handle: Long, operation: String, name: String, custom: Double) {
        phase("native-$operation-$name-default-reset-history") {
            val original = NativeCore.listModules(handle)
            val position = historyPosition(handle)
            val nativeDefault = parameter(module(handle, operation), name).getDouble("default")
            assertTrue(nativeDefault != custom)
            NativeCore.setParameter(handle, operation, name, custom)
            val customized = NativeCore.listModules(handle)
            NativeCore.setParameter(handle, operation, name, nativeDefault)
            assertEquals(nativeDefault, parameter(module(handle, operation), name).getDouble("value"), 1e-7)
            assertEquals(position + 2, historyPosition(handle))
            val reset = NativeCore.listModules(handle)
            NativeCore.undo(handle)
            assertEquals(customized, NativeCore.listModules(handle))
            NativeCore.redo(handle)
            assertEquals(reset, NativeCore.listModules(handle))
            NativeCore.undo(handle)
            NativeCore.undo(handle)
            assertEquals(original, NativeCore.listModules(handle))
            assertEquals(position, historyPosition(handle))
            edits.put(JSONObject().put("operation", operation).put("parameter", name)
                .put("custom", custom).put("nativeDefault", nativeDefault).put("verification", "native-default-reset-history"))
        }
    }

    private fun rejectEdit(handle: Long, operation: String, name: String, value: Double) {
        val modules = NativeCore.listModules(handle)
        val history = NativeCore.history(handle)
        var rejected = false
        try { NativeCore.setParameter(handle, operation, name, value) }
        catch(_: IOException) { rejected = true }
        assertTrue("Reject invalid/unexposed native edit $operation/$name=$value", rejected)
        assertEquals(modules, NativeCore.listModules(handle))
        assertEquals(history, NativeCore.history(handle))
    }

    private fun metadataEdit(handle: Long, operation: String, name: String, requested: Double) {
        phase("native-$operation-$name-metadata-history-roundtrip") {
            val original = NativeCore.listModules(handle)
            val position = historyPosition(handle)
            val item = parameter(module(handle, operation), name)
            val expected = if(item.getString("type") == "float") requested.toFloat().toDouble() else requested
            assertTrue("A roundtrip must request a changed value", item.getDouble("value") != expected)
            NativeCore.setParameter(handle, operation, name, requested)
            assertEquals(expected, parameter(module(handle, operation), name).getDouble("value"), 1e-7)
            assertEquals(position + 1, historyPosition(handle))
            val edited = NativeCore.listModules(handle)
            NativeCore.undo(handle)
            assertEquals(original, NativeCore.listModules(handle))
            assertEquals(position, historyPosition(handle))
            NativeCore.redo(handle)
            assertEquals(edited, NativeCore.listModules(handle))
            assertEquals(position + 1, historyPosition(handle))
            NativeCore.undo(handle)
            assertEquals(original, NativeCore.listModules(handle))
            edits.put(JSONObject().put("operation", operation).put("parameter", name)
                .put("requested", requested).put("stored", expected).put("verification", "metadata-and-history"))
        }
    }

    private fun pixelEdit(handle: Long, operation: String, name: String, value: Double, baseline: ByteArray) {
        val original = module(handle, operation).toString()
        val position = historyPosition(handle)
        phase("apply-$operation-$name") { NativeCore.setParameter(handle, operation, name, value) }
        val changed = module(handle, operation).toString()
        assertEquals(value.toFloat().toDouble(), parameter(module(handle, operation), name).getDouble("value"), 0.0)
        assertEquals(position + 1, historyPosition(handle))
        val editedPixels = phase("preview-$operation-$name") { preview(handle) }
        assertFalse("Original $operation/$name must change real RGB preview pixels", baseline.contentEquals(editedPixels))
        phase("undo-$operation-$name") { NativeCore.undo(handle) }
        assertEquals(original, module(handle, operation).toString())
        assertEquals(position, historyPosition(handle))
        assertTrue("Undo must restore exact preview bytes", baseline.contentEquals(
            phase("preview-undo-$operation-$name") { preview(handle) }))
        phase("redo-$operation-$name") { NativeCore.redo(handle) }
        assertEquals(changed, module(handle, operation).toString())
        assertEquals(position + 1, historyPosition(handle))
        assertTrue("Redo must restore exact edited preview bytes", editedPixels.contentEquals(
            phase("preview-redo-$operation-$name") { preview(handle) }))
        phase("restore-baseline-$operation-$name") { NativeCore.undo(handle) }
        assertEquals(original, module(handle, operation).toString())
        edits.put(JSONObject().put("operation", operation).put("parameter", name).put("requested", value)
            .put("baselinePreviewSha256", sha256(baseline)).put("editedPreviewSha256", sha256(editedPixels))
            .put("verification", "pixels-metadata-and-history"))
        save()
    }

    private fun preview(handle: Long): ByteArray {
        val dimensions = IntArray(2)
        val pixels = NativeCore.renderPreview(handle, 32, 24, dimensions)
        previewCount++
        report.put("previewCount", previewCount)
        assertEquals(32, dimensions[0]); assertEquals(24, dimensions[1])
        assertEquals(32 * 24 * 4, pixels.size)
        return ByteArray(32 * 24 * 3) { index -> pixels[(index / 3) * 4 + index % 3] }
    }

    private fun module(handle: Long, operation: String): JSONObject {
        val modules = JSONArray(NativeCore.listModules(handle))
        return (0 until modules.length()).map(modules::getJSONObject).single { it.getString("operation") == operation }
    }

    private fun parameter(module: JSONObject, name: String): JSONObject = module.getJSONArray("parameters").let { items ->
        (0 until items.length()).map(items::getJSONObject).single { it.getString("name") == name }
    }

    private fun historyPosition(handle: Long) = JSONObject(NativeCore.history(handle)).getInt("position")

    private inline fun <T> phase(name: String, action: () -> T): T {
        report.put("phase", name); save()
        Log.i("NativeAdditionalControls", "phase=$name elapsedMs=${SystemClock.elapsedRealtime() - started}")
        val before = SystemClock.elapsedRealtime()
        val result = action()
        phases.put(JSONObject().put("name", name).put("durationMs", SystemClock.elapsedRealtime() - before))
        save()
        return result
    }

    private fun save() {
        report.put("elapsedMs", SystemClock.elapsedRealtime() - started)
        val output = resultFile.startWrite()
        try { output.write(report.toString(2).toByteArray(Charsets.UTF_8)); resultFile.finishWrite(output) }
        catch(failure: Throwable) { resultFile.failWrite(output); throw failure }
    }

    private fun sha256(file: File) = file.inputStream().buffered().use { input ->
        val digest = MessageDigest.getInstance("SHA-256")
        val buffer = ByteArray(16 * 1024)
        while(true) {
            val count = input.read(buffer)
            if(count < 0) break
            digest.update(buffer, 0, count)
        }
        digest.digest().joinToString("") { "%02x".format(it.toInt() and 0xff) }
    }

    private fun sha256(bytes: ByteArray) = MessageDigest.getInstance("SHA-256").digest(bytes)
        .joinToString("") { "%02x".format(it.toInt() and 0xff) }
}
