/* SPDX-License-Identifier: GPL-3.0-or-later */
package org.example.darktableandroid.nativecore

import android.os.Build
import android.os.SystemClock
import android.util.AtomicFile
import android.util.Log
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.filters.LargeTest
import androidx.test.platform.app.InstrumentationRegistry
import java.io.DataInputStream
import java.io.File
import java.security.MessageDigest
import java.util.UUID
import kotlin.math.abs
import org.json.JSONArray
import org.json.JSONObject
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Assume.assumeTrue
import org.junit.Test
import org.junit.runner.RunWith

/**
 * Original Fusion GPR -> original XCF writer through actual Android JNI.
 * Pass -e requireXcf true to make missing engine/encoder capabilities failures.
 * Header checks establish the requested container/precision, not successful
 * pixel decoding: pull the XCF/TIFF pairs for an independent GIMP comparison.
 */
@RunWith(AndroidJUnit4::class)
@LargeTest
class NativeXcfExportTest {
    private val context get() = InstrumentationRegistry.getInstrumentation().targetContext
    private val report = JSONObject()
    private val phases = JSONArray()
    private val exports = JSONArray()
    private val started = SystemClock.elapsedRealtime()
    private lateinit var resultFile: AtomicFile

    @Test
    fun fusionGprXcfPrecisionsAndReferenceTiffs() {
        val arguments = InstrumentationRegistry.getArguments()
        val required = arguments.getString("requireXcf") == "true"
        val directory = File(context.filesDir, "engine-xcf").apply { check(mkdirs() || isDirectory) }
        resultFile = AtomicFile(File(directory, "result.json"))
        report.put("status", "running").put("phase", "check-engine-capabilities")
            .put("phases", phases).put("exports", exports).put("required", required)
            .put("packageName", context.packageName)
            .put("device", JSONObject().put("sdk", Build.VERSION.SDK_INT)
                .put("abis", JSONArray(Build.SUPPORTED_ABIS.toList())).put("fingerprint", Build.FINGERPRINT))
        save()
        var handle = 0L
        try {
            val capabilities = phase("load-native-capabilities") { NativeCore.capabilities() }
            report.put("capabilities", capabilities)
            if(!required && capabilities and 3 != 3) {
                report.put("status", "skipped").put("reason", "Real engine/VC-5 unavailable; use requireXcf=true to require it")
                save()
                assumeTrue("A real engine and VC-5 decoder are required", false)
            }
            assertEquals("A required XCF test must contain the real engine and VC-5 decoder", 3, capabilities and 3)
            val input = phase("validate-official-fusion-fixture") {
                File(requireNotNull(arguments.getString("inputFile")) {
                    "Supply -e inputFile with original GPBK7066.GPR inside the target app's private files directory"
                }).canonicalFile.also {
                    require(it.toPath().startsWith(context.filesDir.canonicalFile.toPath()) && it.isFile)
                    assertEquals("Use the pinned original official Fusion fixture", FUSION_SHA256, sha256(it))
                }
            }
            report.put("inputFile", input.absolutePath).put("inputBytes", input.length())
                .put("sourceSha256Before", FUSION_SHA256).put("sourceWidth", 3104).put("sourceHeight", 3000)
            val outputDirectory = File(directory, "run-${UUID.randomUUID()}").apply { check(mkdir()) }
            report.put("outputDirectory", outputDirectory.absolutePath)
            phase("extract-and-configure-runtime") {
                RuntimeAssets.initializeNative(context)
                report.put("nativeLibraryDir", context.applicationInfo.nativeLibraryDir)
            }
            phase("open-original-fusion-gpr") {
                handle = NativeCore.open(input.absolutePath)
                assertTrue("Native open must return a live session", handle != 0L)
            }
            val formats = JSONArray(phase("enumerate-native-export-formats") { NativeCore.listExportFormats(handle) })
            val installed = (0 until formats.length()).map(formats::getJSONObject)
            val xcf = installed.singleOrNull { it.optString("id", it.optString("name")) == "xcf" }
            if(!required && xcf == null) {
                phase("close-before-skip") { NativeCore.close(handle); handle = 0L }
                report.put("status", "skipped").put("reason", "Original XCF module unavailable; use requireXcf=true to require it")
                save()
                assumeTrue("The installed runtime does not contain XCF", false)
            }
            checkNotNull(xcf) { "The required original XCF module is not installed" }
            assertEquals("image/x-xcf", xcf.getString("mimeType"))
            val options = xcf.getJSONObject("options")
            val precisions = options.getJSONArray("pixelFormats")
            assertEquals(listOf(0, 1, 2, 3), (0 until precisions.length()).map { precisions.getJSONObject(it).getInt("value") })
            val profiles = options.getJSONArray("profiles")
            assertTrue("The original sRGB output profile must be available", (0 until profiles.length()).any {
                profiles.getJSONObject(it).getInt("value") == 1
            })
            assertTrue("Reference TIFF module must be installed", installed.any { it.optString("id", it.optString("name")) == "tiff" })
            report.put("xcfFormat", xcf)
            val modules = JSONArray(NativeCore.listModules(handle))
            val exposure = (0 until modules.length()).map(modules::getJSONObject)
                .single { it.getString("operation") == "exposure" }.getJSONArray("parameters")
            val parameter = (0 until exposure.length()).map(exposure::getJSONObject)
                .single { it.getString("name") == "exposure" }
            val original = parameter.getDouble("value")
            val edited = if(original + 0.5 <= parameter.getDouble("max")) original + 0.5 else original - 0.5
            assertTrue(edited >= parameter.getDouble("min"))
            phase("apply-original-exposure-module") { NativeCore.setParameter(handle, "exposure", "exposure", edited) }
            report.put("exposureBefore", original).put("exposureAfter", edited)
            for((pixelFormat, precision) in listOf(1 to 150, 2 to 250, 3 to 650)) {
                val settings = ExportSettings(maxWidth = 128, maxHeight = 128, pixelFormat = pixelFormat,
                    outputProfile = 1, metadataPolicy = 1)
                val output = File(outputDirectory, "edited-$pixelFormat.xcf")
                phase("native-xcf-export-$pixelFormat") {
                    NativeCore.exportWithSettings(handle, output.absolutePath, "xcf", settings.toNativeArray())
                }
                val header = phase("validate-xcf-header-$pixelFormat") { xcfHeader(output, precision) }
                val reference = File(outputDirectory, "reference-$pixelFormat.tif")
                phase("native-reference-tiff-export-$pixelFormat") {
                    NativeCore.exportWithSettings(handle, reference.absolutePath, "tiff", settings.toNativeArray())
                }
                assertTrue("Reference TIFF must be nonempty", reference.isFile && reference.length() > 8)
                exports.put(header.put("pixelFormat", pixelFormat).put("path", output.absolutePath)
                    .put("bytes", output.length()).put("sha256", sha256(output))
                    .put("referenceTiff", JSONObject().put("path", reference.absolutePath)
                        .put("bytes", reference.length()).put("sha256", sha256(reference))))
                save()
            }
            phase("source-hash-after-exports") {
                val after = sha256(input)
                report.put("sourceSha256After", after)
                assertEquals("Original GPR must stay unchanged", FUSION_SHA256, after)
            }
            phase("close-native-session") { NativeCore.close(handle); handle = 0L }
            phase("source-hash-after-close") {
                val after = sha256(input)
                report.put("sourceSha256AfterFinalClose", after)
                assertEquals("Closing the session must preserve the source", FUSION_SHA256, after)
            }
            report.put("status", "passed").put("phase", "complete")
                .put("independentPixelDecode", "pending host GIMP validation of exported XCF/TIFF pairs")
            save()
            Log.i(TAG, "passed report=${resultFile.baseFile.absolutePath}")
        } catch(failure: org.junit.AssumptionViolatedException) {
            throw failure
        } catch(failure: Throwable) {
            report.put("status", "failed").put("failure", Log.getStackTraceString(failure))
            save()
            throw failure
        } finally {
            if(handle != 0L) {
                try { NativeCore.close(handle) }
                catch(failure: Throwable) {
                    report.put("closeFailure", Log.getStackTraceString(failure))
                    save()
                }
            }
        }
    }

    private fun xcfHeader(file: File, precision: Int): JSONObject {
        assertTrue("Original encoder must write a nonempty XCF", file.isFile && file.length() > 30)
        return DataInputStream(file.inputStream().buffered()).use { input ->
            val magic = ByteArray(14).also(input::readFully)
            assertTrue("Original writer must emit XCF v012", magic.contentEquals("gimp xcf v012\u0000".toByteArray(Charsets.US_ASCII)))
            val width = input.readInt()
            val height = input.readInt()
            assertTrue("Bounded export dimensions", width in 1..128 && height in 1..128 && maxOf(width, height) == 128)
            assertTrue("Fusion aspect must be preserved within rounding", abs(width.toDouble() / height - 3104.0 / 3000) < 0.02)
            assertEquals("RGB XCF", 0, input.readInt())
            assertEquals("Original writer must honor requested gamma precision", precision, input.readInt())
            JSONObject().put("version", 12).put("width", width).put("height", height)
                .put("baseType", "RGB").put("precision", precision)
        }
    }

    private inline fun <T> phase(name: String, operation: () -> T): T {
        report.put("phase", name)
        save()
        Log.i(TAG, "phase=$name elapsedMs=${SystemClock.elapsedRealtime() - started}")
        val before = SystemClock.elapsedRealtime()
        val result = operation()
        phases.put(JSONObject().put("name", name).put("durationMs", SystemClock.elapsedRealtime() - before))
        save()
        return result
    }

    private fun save() {
        report.put("elapsedMs", SystemClock.elapsedRealtime() - started)
        val stream = resultFile.startWrite()
        try {
            stream.write(report.toString(2).toByteArray(Charsets.UTF_8))
            resultFile.finishWrite(stream)
        } catch(failure: Throwable) {
            resultFile.failWrite(stream)
            throw failure
        }
    }

    companion object {
        private const val TAG = "NativeXcfExport"
        private const val FUSION_SHA256 = "c03eccdc334dc5b35cf6385799b219f4c9a0bc18ccbe70fe433237d98f352247"

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
    }
}
