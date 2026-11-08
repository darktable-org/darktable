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

/** Original colisa/colorize processing through JNI on a small RGB input.
 * Bitmap generates only the lossless input fixture; all edits use the native
 * desktop modules. This is separate from RAW/GPR and Compose interaction tests. */
@RunWith(AndroidJUnit4::class)
@LargeTest
class NativeColorControlsTest {
    private val context get() = InstrumentationRegistry.getInstrumentation().targetContext
    private val report = JSONObject()
    private val phases = JSONArray()
    private val edits = JSONArray()
    private val started = SystemClock.elapsedRealtime()
    private lateinit var resultFile: AtomicFile

    @Test
    fun originalColorControlsPixelsAndHistory() {
        val required = InstrumentationRegistry.getArguments().getString("requireColorControls") == "true"
        val directory = File(context.filesDir, "color-controls").apply { check(mkdirs() || isDirectory) }
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
                report.put("status", "skipped").put("reason", "Real engine absent; requireColorControls=true makes this mandatory")
                save()
                assumeTrue("Requires the original native engine", false)
            }
            assertEquals("Requires the original native engine", 1, capabilities and 1)
            phase("configure-original-runtime") { RuntimeAssets.initializeNative(context) }
            phase("open-png-through-original-engine") {
                handle = NativeCore.open(input.absolutePath)
                assertTrue(handle != 0L)
            }
            phase("verify-native-color-control-metadata") {
                verifyFields(handle, "colisa", mapOf("contrast" to (-1.0 to 1.0),
                    "brightness" to (-1.0 to 1.0), "saturation" to (-1.0 to 1.0)))
                verifyFields(handle, "colorize", mapOf("hue" to (0.0 to 1.0),
                    "saturation" to (0.0 to 1.0), "source_lightness_mix" to (0.0 to 100.0),
                    "lightness" to (0.0 to 100.0)))
                assertFalse(fields(module(handle, "colorize")).any { it.getString("name") == "version" })
                rejectEdit(handle, "colorize", "version", 2.0)
            }
            phase("discover-native-parameters-in-mobile-model") {
                val models = parseModules(NativeCore.listModules(handle))
                assertEquals(3, models.single { it.operation == "colisa" }.parameters.size)
                assertEquals(4, models.single { it.operation == "colorize" }.parameters.size)
                assertEquals("exposure", models.initialControlModule())
                val ordered = models.controlsFirst()
                assertTrue(ordered.dropWhile { it.hasEditableControls }.none { it.hasEditableControls })
            }

            // Enabling is its own history step, so each tested parameter starts
            // from a fully enabled, original-module baseline.
            phase("enable-colisa") { NativeCore.setModuleEnabled(handle, "colisa", true) }
            // A host PNG fixture showed a one-time first-render difference
            // before edits. Prime explicitly, then require exact repeatability.
            phase("prime-original-png-preview") { preview(handle) }
            val colisaBaseline = phase("preview-colisa-baseline") { preview(handle) }
            assertTrue("Unedited baseline must be stable", colisaBaseline.contentEquals(
                phase("verify-colisa-baseline-stability") { preview(handle) }))
            for((name, value) in listOf("contrast" to 0.5, "brightness" to 0.25, "saturation" to -0.5)) {
                checkPixelEdit(handle, "colisa", name, value, colisaBaseline)
            }
            phase("disable-colisa") { NativeCore.setModuleEnabled(handle, "colisa", false) }
            phase("enable-colorize") { NativeCore.setModuleEnabled(handle, "colorize", true) }
            val colorizeBaseline = phase("preview-colorize-baseline") { preview(handle) }
            for((name, value) in listOf("hue" to 0.25, "saturation" to 0.75)) {
                checkPixelEdit(handle, "colorize", name, value, colorizeBaseline)
            }
            // Check both remaining native setters and topology/history without
            // more expensive pixelpipes; the five edits above verify pixels.
            for((name, value) in listOf("source_lightness_mix" to 25.0, "lightness" to 62.5)) {
                phase("native-colorize-$name-metadata-roundtrip") {
                    val baseline = module(handle, "colorize").toString()
                    val position = historyPosition(handle)
                    NativeCore.setParameter(handle, "colorize", name, value)
                    assertEquals(value, parameter(module(handle, "colorize"), name).getDouble("value"), 0.0)
                    assertEquals(position + 1, historyPosition(handle))
                    val edited = module(handle, "colorize").toString()
                    NativeCore.undo(handle)
                    assertEquals(baseline, module(handle, "colorize").toString())
                    assertEquals(position, historyPosition(handle))
                    NativeCore.redo(handle)
                    assertEquals(edited, module(handle, "colorize").toString())
                    assertEquals(position + 1, historyPosition(handle))
                    NativeCore.undo(handle)
                }
            }
            phase("source-hash-after-color-edits") {
                report.put("sourceSha256After", sha256(input))
                assertEquals(before, sha256(input))
            }
            phase("close-native-color-session") { NativeCore.close(handle); handle = 0L }
            phase("source-hash-after-close") {
                report.put("sourceSha256AfterFinalClose", sha256(input))
                assertEquals(before, sha256(input))
            }
            report.put("status", "passed").put("phase", "complete").put("previewCount", 19)
                .put("previewRgbBytes", 32 * 24 * 3)
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

    private fun verifyFields(handle: Long, operation: String, expected: Map<String, Pair<Double, Double>>) {
        val items = fields(module(handle, operation))
        assertEquals(expected.keys, items.map { it.getString("name") }.toSet())
        assertEquals(expected.size, items.size)
        for(item in items) {
            val name = item.getString("name")
            assertEquals("float", item.getString("type"))
            assertEquals(expected.getValue(name).first, item.getDouble("min"), 0.0)
            assertEquals(expected.getValue(name).second, item.getDouble("max"), 0.0)
            assertTrue(item.getDouble("value").isFinite())
            rejectEdit(handle, operation, name, item.getDouble("max") + 1.0)
            rejectEdit(handle, operation, name, item.getDouble("min") - 1.0)
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

    private fun checkPixelEdit(handle: Long, operation: String, name: String, value: Double, baseline: ByteArray) {
        val original = module(handle, operation).toString()
        val position = historyPosition(handle)
        phase("apply-$operation-$name") { NativeCore.setParameter(handle, operation, name, value) }
        val changed = module(handle, operation).toString()
        assertEquals(value, parameter(module(handle, operation), name).getDouble("value"), 0.0)
        assertEquals(position + 1, historyPosition(handle))
        val editedPixels = phase("preview-$operation-$name") { preview(handle) }
        assertFalse("Original $operation/$name must change real RGB preview pixels", baseline.contentEquals(editedPixels))
        phase("undo-$operation-$name") { NativeCore.undo(handle) }
        assertEquals(original, module(handle, operation).toString())
        assertEquals(position, historyPosition(handle))
        assertTrue("Undo must restore exact preview bytes", baseline.contentEquals(phase("preview-undo-$operation-$name") { preview(handle) }))
        phase("redo-$operation-$name") { NativeCore.redo(handle) }
        assertEquals(changed, module(handle, operation).toString())
        assertEquals(position + 1, historyPosition(handle))
        assertTrue("Redo must restore exact edited preview bytes", editedPixels.contentEquals(phase("preview-redo-$operation-$name") { preview(handle) }))
        phase("restore-baseline-$operation-$name") { NativeCore.undo(handle) }
        assertEquals(original, module(handle, operation).toString())
        edits.put(JSONObject().put("operation", operation).put("parameter", name).put("requested", value)
            .put("baselinePreviewSha256", sha256(baseline)).put("editedPreviewSha256", sha256(editedPixels)))
        save()
    }

    private fun preview(handle: Long): ByteArray {
        val dimensions = IntArray(2)
        val pixels = NativeCore.renderPreview(handle, 32, 24, dimensions)
        assertEquals(32, dimensions[0]); assertEquals(24, dimensions[1])
        assertEquals(32 * 24 * 4, pixels.size)
        // Compare color bytes, so an alpha-only difference cannot satisfy the
        // requirement that a real color adjustment changed the image.
        return ByteArray(32 * 24 * 3) { index -> pixels[(index / 3) * 4 + index % 3] }
    }

    private fun module(handle: Long, operation: String): JSONObject {
        val modules = JSONArray(NativeCore.listModules(handle))
        return (0 until modules.length()).map(modules::getJSONObject).single { it.getString("operation") == operation }
    }

    private fun fields(module: JSONObject): List<JSONObject> = module.getJSONArray("parameters").let { items ->
        (0 until items.length()).map(items::getJSONObject)
    }

    private fun parameter(module: JSONObject, name: String): JSONObject = fields(module).single { it.getString("name") == name }
    private fun historyPosition(handle: Long) = JSONObject(NativeCore.history(handle)).getInt("position")

    private inline fun <T> phase(name: String, action: () -> T): T {
        report.put("phase", name); save()
        Log.i("NativeColorControls", "phase=$name elapsedMs=${SystemClock.elapsedRealtime() - started}")
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
