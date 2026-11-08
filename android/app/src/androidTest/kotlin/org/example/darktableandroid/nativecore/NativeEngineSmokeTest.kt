/* SPDX-License-Identifier: GPL-3.0-or-later */
package org.example.darktableandroid.nativecore

import android.graphics.BitmapFactory
import android.os.Build
import android.os.SystemClock
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
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertTrue
import org.junit.Test
import org.junit.runner.RunWith

/**
 * Real Android/JNI processing test. Supply an original RAW/GPR in the target
 * application's private files directory; no preview, decoder mock or replacement
 * colour calculation is used. The report is written before every native phase,
 * so a linker failure or native process crash leaves a useful last-phase marker.
 */
@RunWith(AndroidJUnit4::class)
@LargeTest
class NativeEngineSmokeTest {
    private val context get() = InstrumentationRegistry.getInstrumentation().targetContext
    private val report = JSONObject()
    private val phases = JSONArray()
    private val started = SystemClock.elapsedRealtime()
    private lateinit var resultFile: File

    @Test
    fun rawEditUndoExportAndReopen() {
        val arguments = InstrumentationRegistry.getArguments()
        val directory = File(context.filesDir, "engine-smoke").apply { check(mkdirs() || isDirectory) }
        resultFile = File(directory, "result.json")
        report.put("status", "running").put("phases", phases)
            .put("phase", "validate-fixture-arguments")
            .put("packageName", context.packageName)
            .put("device", JSONObject().put("sdk", Build.VERSION.SDK_INT)
                .put("abis", JSONArray(Build.SUPPORTED_ABIS.toList())).put("fingerprint", Build.FINGERPRINT))
        save()
        var handle = 0L
        try {
            val outputDirectory = File(directory, "run-${UUID.randomUUID()}").apply { check(mkdir()) }
            val input = File(requireNotNull(arguments.getString("inputFile")) {
                "Supply -e inputFile with an original RAW/GPR in the target app's files directory"
            }).canonicalFile
            require(input.toPath().startsWith(context.filesDir.canonicalFile.toPath()) && input.isFile) {
                "inputFile must be a regular file within the target app's private files directory"
            }
            val expectedWidth = requireNotNull(arguments.getString("expectedWidth")?.toIntOrNull()) {
                "Supply -e expectedWidth from the pinned official source sample"
            }
            val expectedHeight = requireNotNull(arguments.getString("expectedHeight")?.toIntOrNull()) {
                "Supply -e expectedHeight from the pinned official source sample"
            }
            require(expectedWidth > 192 && expectedHeight > 192)
            report.put("inputFile", input.absolutePath).put("inputBytes", input.length())
                .put("expectedWidth", expectedWidth).put("expectedHeight", expectedHeight)
            val sourceHash = phase("source-hash-before") { sha256(input) }
            report.put("sourceSha256Before", sourceHash)
            phase("load-native-capabilities") {
                val capabilities = NativeCore.capabilities()
                report.put("capabilities", capabilities)
                assertEquals("The installed JNI library must contain the real engine and VC-5 decoder", 3, capabilities and 3)
            }
            phase("extract-and-configure-runtime") {
                RuntimeAssets.initializeNative(context)
                report.put("nativeLibraryDir", context.applicationInfo.nativeLibraryDir)
            }
            phase("open-original-raw") {
                handle = NativeCore.open(input.absolutePath)
                assertTrue("Native open must return a live session", handle != 0L)
            }
            val baseline = phase("preview-original-raw") { preview(handle) }
            report.put("baselinePreview", baseline.json())

            val exposure = module(handle, "exposure")
            val parameters = exposure.getJSONArray("parameters")
            val exposureParameter = (0 until parameters.length()).map(parameters::getJSONObject)
                .single { it.getString("name") == "exposure" }
            val originalExposure = exposureParameter.getDouble("value")
            val maximum = exposureParameter.getDouble("max")
            val editedExposure = if(originalExposure + 1.0 <= maximum) originalExposure + 1.0 else originalExposure - 1.0
            assertTrue(editedExposure >= exposureParameter.getDouble("min"))
            phase("apply-exposure") { NativeCore.setParameter(handle, "exposure", "exposure", editedExposure) }
            val exposed = phase("preview-exposure") { preview(handle) }
            assertDifferent("Exposure must change actual decoded RAW pixels", baseline, exposed)
            phase("undo-exposure") { NativeCore.undo(handle) }
            assertSame("Undo must restore the original pixels", baseline, phase("preview-exposure-undo") { preview(handle) })
            phase("redo-exposure") { NativeCore.redo(handle) }
            assertSame("Redo must restore the edited pixels", exposed, phase("preview-exposure-redo") { preview(handle) })
            phase("restore-exposure-baseline") { NativeCore.undo(handle) }

            verifyNewDefaultInstance(handle, baseline)

            val balance = module(handle, "temperature").getJSONObject("whiteBalance")
            val originalTemperature = balance.getJSONObject("temperature")
            val originalKelvin = originalTemperature.getDouble("kelvin")
            val kelvin = if(kotlin.math.abs(originalKelvin - 6500.0) > 100.0) 6500.0 else 5200.0
            val tint = 1.1
            report.put("requestedWhiteBalance", JSONObject().put("kelvin", kelvin).put("tint", tint))
            phase("apply-native-kelvin-tint") { NativeCore.setWhiteBalanceTemperature(handle, "temperature", kelvin, tint) }
            val changedBalance = module(handle, "temperature").getJSONObject("whiteBalance")
            assertEquals("Kelvin/tint must select the original user-defined setting", 2, changedBalance.getInt("setting"))
            report.put("whiteBalanceAfter", changedBalance)
            val balanced = phase("preview-kelvin-tint") { preview(handle) }
            assertDifferent("Native Kelvin/tint must change actual pixels", baseline, balanced)
            phase("undo-kelvin-tint") { NativeCore.undo(handle) }
            assertSame("Kelvin/tint undo must restore original pixels", baseline, phase("preview-kelvin-tint-undo") { preview(handle) })
            phase("redo-kelvin-tint") { NativeCore.redo(handle) }
            assertSame("Kelvin/tint redo must restore edited pixels", balanced, phase("preview-kelvin-tint-redo") { preview(handle) })

            val formats = JSONArray(phase("enumerate-native-export-formats") { NativeCore.listExportFormats(handle) })
            assertTrue("The original PNG format module must be installed", (0 until formats.length()).any {
                val item = formats.getJSONObject(it)
                item.optString("id", item.optString("name")) == "png"
            })
            val output = File(outputDirectory, "edited-full-size.png")
            phase("native-full-size-png-export") { NativeCore.exportImage(handle, output.absolutePath, "png", 95) }
            assertTrue("The native encoder must produce a nonempty PNG", output.isFile && output.length() > 24)
            phase("android-decode-full-size-png") {
                val dimensions = BitmapFactory.Options().apply { inJustDecodeBounds = true }
                BitmapFactory.decodeFile(output.absolutePath, dimensions)
                assertEquals("The native PNG module must produce PNG bytes", "image/png", dimensions.outMimeType)
                assertEquals("Full-size PNG width", expectedWidth, dimensions.outWidth)
                assertEquals("Full-size PNG height", expectedHeight, dimensions.outHeight)
                val decoded = BitmapFactory.decodeFile(output.absolutePath)
                assertNotNull("Android must decode the completed native PNG", decoded)
                checkNotNull(decoded).let { bitmap ->
                    try {
                        assertEquals(expectedWidth, bitmap.width)
                        assertEquals(expectedHeight, bitmap.height)
                        report.put("export", JSONObject().put("path", output.absolutePath)
                            .put("bytes", output.length()).put("width", bitmap.width).put("height", bitmap.height)
                            .put("bitmapConfig", bitmap.config?.name).put("sha256", sha256(output)))
                    } finally {
                        bitmap.recycle()
                    }
                }
            }
            phase("source-hash-after-export") {
                val after = sha256(input)
                report.put("sourceSha256After", after)
                assertEquals("Editing/export must not change the original RAW/GPR", sourceHash, after)
            }
            phase("close-first-session") {
                NativeCore.close(handle)
                handle = 0L
            }
            phase("reconfigure-identical-runtime") { RuntimeAssets.initializeNative(context) }
            phase("reopen-original-raw") {
                handle = NativeCore.open(input.absolutePath)
                assertTrue(handle != 0L)
            }
            assertSame("Reopen must start with the unchanged original image", baseline, phase("preview-reopened-raw") { preview(handle) })
            phase("close-reopened-session") {
                NativeCore.close(handle)
                handle = 0L
            }
            phase("source-hash-after-final-close") {
                val after = sha256(input)
                report.put("sourceSha256AfterFinalClose", after)
                assertEquals("Closing and reopening must not change the original RAW/GPR", sourceHash, after)
            }
            report.put("status", "passed").put("phase", "complete")
            save()
            Log.i(TAG, "passed report=${resultFile.absolutePath}")
        } catch(failure: Throwable) {
            report.put("status", "failed").put("failure", Log.getStackTraceString(failure))
            save()
            throw failure
        } finally {
            if(handle != 0L) {
                try {
                    NativeCore.close(handle)
                } catch(closeFailure: Throwable) {
                    report.put("closeFailure", Log.getStackTraceString(closeFailure))
                    save()
                }
            }
        }
    }

    private data class Preview(val width: Int, val height: Int, val pixels: ByteArray) {
        fun json(): JSONObject = JSONObject().put("width", width).put("height", height).put("sha256", digest(pixels))
    }

    private fun preview(handle: Long): Preview {
        val dimensions = IntArray(2)
        val pixels = NativeCore.renderPreview(handle, 192, 192, dimensions)
        assertTrue("Preview must have positive bounded dimensions", dimensions.all { it in 1..192 })
        assertEquals("Preview must contain actual RGBA pixels", dimensions[0] * dimensions[1] * 4, pixels.size)
        return Preview(dimensions[0], dimensions[1], pixels)
    }

    private fun module(handle: Long, operation: String): JSONObject {
        return modules(handle).single { it.getString("operation") == operation }
    }

    private fun modules(handle: Long): List<JSONObject> {
        val items = JSONArray(NativeCore.listModules(handle))
        return (0 until items.length()).map(items::getJSONObject)
    }

    private fun parameter(module: JSONObject, name: String): JSONObject {
        val items = module.getJSONArray("parameters")
        return (0 until items.length()).map(items::getJSONObject).single { it.getString("name") == name }
    }

    private fun verifyNewDefaultInstance(handle: Long, baseline: Preview) {
        val before = modules(handle)
        val originalIds = before.map { it.getString("operation") }
        val source = before.single { it.getString("operation") == "exposure" }
        phase("require-default-instance-capability") {
            assertTrue("This smoke test requires a new-instance-capable native engine",
                source.opt("canCreateInstance") == true)
        }
        val sourceExposure = parameter(source, "exposure").getDouble("value")
        val identifier = phase("create-default-exposure-instance") { NativeCore.newInstance(handle, "exposure") }
        assertTrue("The native owner must return a distinct instance identifier",
            identifier.isNotEmpty() && identifier !in originalIds)
        val created = module(handle, identifier)
        assertEquals(before.size + 1, modules(handle).size)
        assertTrue("A new default instance follows desktop enabled semantics", created.getBoolean("enabled"))
        val defaults = parameter(created, "exposure")
        val defaultExposure = defaults.getDouble("value")
        report.put("newDefaultInstance", JSONObject().put("identifier", identifier)
            .put("defaultExposure", defaultExposure).put("module", created))
        val createdPreview = phase("preview-default-exposure-instance") { preview(handle) }
        val editedExposure = if(defaultExposure + 1.0 <= defaults.getDouble("max"))
            defaultExposure + 1.0 else defaultExposure - 1.0
        assertTrue(editedExposure >= defaults.getDouble("min"))
        phase("edit-default-exposure-instance") {
            NativeCore.setParameter(handle, identifier, "exposure", editedExposure)
        }
        assertEquals("Editing an additional instance must leave the source instance unchanged",
            sourceExposure, parameter(module(handle, "exposure"), "exposure").getDouble("value"), 0.0)
        val editedPreview = phase("preview-edited-exposure-instance") { preview(handle) }
        assertDifferent("Editing the new instance must change actual RAW pixels", createdPreview, editedPreview)
        report.getJSONObject("newDefaultInstance").put("defaultPreview", createdPreview.json())
            .put("editedPreview", editedPreview.json())
        phase("undo-additional-instance-edit") { NativeCore.undo(handle) }
        assertSame("Undo must restore the new instance's default pixels", createdPreview,
            phase("preview-additional-instance-edit-undo") { preview(handle) })
        phase("undo-additional-instance-creation") { NativeCore.undo(handle) }
        assertEquals("Undo must restore the original active module topology", originalIds,
            modules(handle).map { it.getString("operation") })
        assertSame("Undoing creation must restore the original pixels", baseline,
            phase("preview-additional-instance-creation-undo") { preview(handle) })
        phase("reject-retired-instance-selector") {
            val history = NativeCore.history(handle)
            val failure = runCatching { NativeCore.newInstance(handle, identifier) }.exceptionOrNull()
            assertTrue("JNI must reject a retired instance selector with an IO error", failure is IOException)
            assertEquals("A rejected selector must not alter native undo/redo", history, NativeCore.history(handle))
        }
        phase("redo-additional-instance-creation") { NativeCore.redo(handle) }
        assertEquals("Redo must restore the same stable instance ID", before.size + 1, modules(handle).size)
        assertEquals(defaultExposure, parameter(module(handle, identifier), "exposure").getDouble("value"), 0.0)
        assertSame("Redoing creation must restore the default instance pixels", createdPreview,
            phase("preview-additional-instance-creation-redo") { preview(handle) })
        phase("restore-before-additional-instance") { NativeCore.undo(handle) }
        assertEquals(originalIds, modules(handle).map { it.getString("operation") })
    }

    private fun assertSame(message: String, expected: Preview, actual: Preview) {
        assertEquals(message, expected.width, actual.width)
        assertEquals(message, expected.height, actual.height)
        assertTrue(message, expected.pixels.contentEquals(actual.pixels))
    }

    private fun assertDifferent(message: String, original: Preview, edited: Preview) {
        assertEquals(message, original.width, edited.width)
        assertEquals(message, original.height, edited.height)
        assertFalse(message, original.pixels.contentEquals(edited.pixels))
    }

    private inline fun <T> phase(name: String, operation: () -> T): T {
        report.put("phase", name)
        save()
        Log.i(TAG, "phase=$name elapsedMs=${SystemClock.elapsedRealtime() - started}")
        val phaseStarted = SystemClock.elapsedRealtime()
        val value = operation()
        phases.put(JSONObject().put("name", name).put("durationMs", SystemClock.elapsedRealtime() - phaseStarted))
        save()
        return value
    }

    private fun save() {
        report.put("elapsedMs", SystemClock.elapsedRealtime() - started)
        resultFile.writeText(report.toString(2))
    }

    companion object {
        private const val TAG = "NativeEngineSmoke"

        private fun sha256(file: File): String {
            val digest = MessageDigest.getInstance("SHA-256")
            file.inputStream().buffered().use { input ->
                val buffer = ByteArray(64 * 1024)
                while(true) {
                    val count = input.read(buffer)
                    if(count < 0) break
                    digest.update(buffer, 0, count)
                }
            }
            return digest.digest().joinToString("") { "%02x".format(it.toInt() and 0xff) }
        }

        private fun digest(bytes: ByteArray): String = MessageDigest.getInstance("SHA-256").digest(bytes)
            .joinToString("") { "%02x".format(it.toInt() and 0xff) }
    }
}
