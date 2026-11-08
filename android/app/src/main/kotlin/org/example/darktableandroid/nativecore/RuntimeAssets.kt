/* SPDX-License-Identifier: GPL-3.0-or-later */
package org.example.darktableandroid.nativecore

import android.content.Context
import android.content.res.AssetManager
import android.os.Build
import java.io.File
import java.io.IOException
import java.util.Locale

data class RuntimeDirectories(val dataDir: File, val moduleDir: File, val tempRoot: File)

/** Prepares data only. Native code must remain in Android's installed library directory. */
object RuntimeAssets {
    private const val ASSET_ROOT = "darktable-runtime"
    private const val ASSET_DATA = "$ASSET_ROOT/data"
    private const val COMPLETE = ".complete"
    private val requiredFiles = listOf("darktablerc", "rawspeed/cameras.xml", "noiseprofiles.json", "wb_presets.json")
    private val nativeConfiguration = RuntimeConfiguration()

    /** Reuses an identical, successful process configuration across ViewModels. */
    fun initializeNative(context: Context) {
        val runtime = prepare(context)
        nativeConfiguration.configure(RuntimePaths(runtime.dataDir.absolutePath, runtime.moduleDir.absolutePath,
            runtime.tempRoot.absolutePath)) { paths ->
            NativeCore.configureRuntime(paths.data, paths.modules, paths.temporary)
        }
    }

    /** Call on an IO dispatcher, after checking that the native engine is present. */
    @Synchronized
    fun prepare(context: Context): RuntimeDirectories {
        val application = context.applicationContext
        val assets = application.assets
        // These are necessary core inputs, not proof that every optional module's
        // profiles, models, presets and other data have been packaged correctly.
        requiredFiles.forEach { path ->
            try {
                assets.open("$ASSET_DATA/$path").use { input ->
                    if(input.read() < 0) throw IOException("Runtime asset is empty: $ASSET_DATA/$path")
                }
            } catch(failure: IOException) {
                throw IOException(
                    "Complete darktable runtime data is missing from assets/$ASSET_DATA ($path). " +
                        "The standalone camera, noise and white-balance databases are insufficient.", failure,
                )
            }
        }
        val hasTranslations = assets.list("$ASSET_ROOT/locale").orEmpty().any { language ->
            runCatching {
                assets.open("$ASSET_ROOT/locale/$language/LC_MESSAGES/darktable.mo").use { it.read() >= 0 }
            }.getOrDefault(false)
        }
        if(!hasTranslations) throw IOException("Complete darktable runtime translations are missing from assets/$ASSET_ROOT/locale")

        val moduleDir = File(application.applicationInfo.nativeLibraryDir).canonicalFile
        if(!moduleDir.isDirectory) {
            throw IOException("Android did not extract the native processing modules into nativeLibraryDir")
        }
        val filesRoot = application.filesDir.canonicalFile
        val runtimeRoot = checkedChild(filesRoot, "darktable-runtime")
        makeDirectory(runtimeRoot)
        val cacheRoot = application.cacheDir.canonicalFile
        val tempRoot = checkedChild(cacheRoot, "darktable-runtime")
        makeDirectory(tempRoot)

        // Reinstalling a debug APK without changing versionCode also changes the
        // package installation stamp. Never merge a new data tree over an older
        // one, where files removed by an update could otherwise remain active.
        val packageInfo = application.packageManager.getPackageInfo(application.packageName, 0)
        @Suppress("DEPRECATION")
        val versionCode = if(Build.VERSION.SDK_INT >= 28) packageInfo.longVersionCode else packageInfo.versionCode.toLong()
        val apk = File(application.applicationInfo.sourceDir)
        val stamp = "v1-$versionCode-${packageInfo.lastUpdateTime}-${apk.length()}-${apk.lastModified()}"
        var destination = checkedChild(runtimeRoot, stamp)
        if(isComplete(destination, stamp)) return RuntimeDirectories(File(destination, "data"), moduleDir, tempRoot)
        runtimeRoot.listFiles()?.firstOrNull { it.name.startsWith("$stamp-") && isComplete(it, stamp) }?.let {
            return RuntimeDirectories(File(it, "data"), moduleDir, tempRoot)
        }

        // A failed older extraction is never reused or merged. A distinct repair
        // directory also avoids deleting data an existing native session may own.
        if(destination.exists()) destination = checkedChild(runtimeRoot, "$stamp-${java.util.UUID.randomUUID()}")
        val staging = File.createTempFile("staging-", ".tmp", runtimeRoot)
        if(!staging.delete() || !staging.mkdir()) throw IOException("Unable to create private runtime staging directory")
        try {
            // Keep data's sibling resources too: Lensfun discovers its database
            // relative to the parent of datadir, rather than inside data/.
            copyTree(assets, ASSET_ROOT, staging)
            validateRuntime(staging)
            File(staging, COMPLETE).writeText(stamp)
            if(!staging.renameTo(destination)) throw IOException("Unable to commit complete runtime data")
        } finally {
            if(staging.exists()) staging.deleteRecursively()
        }
        return RuntimeDirectories(File(destination, "data"), moduleDir, tempRoot)
    }

    private fun isComplete(directory: File, stamp: String): Boolean =
        runCatching {
            directory.isDirectory && File(directory, COMPLETE).readText() == stamp &&
                validateRuntime(directory)
        }.getOrDefault(false)

    private fun validateRuntime(directory: File): Boolean {
        val dataDir = checkedChild(directory, "data")
        requiredFiles.forEach { relative ->
            val file = relative.split('/').fold(dataDir) { parent, name -> checkedChild(parent, name) }
            if(!file.isFile || file.length() == 0L) throw IOException("Required runtime data is missing or empty: $relative")
        }
        val localeDir = checkedChild(directory, "locale")
        if(localeDir.listFiles().orEmpty().none { language ->
                val catalog = File(language, "LC_MESSAGES/darktable.mo")
                language.isDirectory && catalog.isFile && catalog.length() > 0
            }) throw IOException("Required runtime translations are missing or empty: locale")
        return true
    }

    private fun copyTree(assets: AssetManager, assetPath: String, destination: File) {
        val children = assets.list(assetPath) ?: throw IOException("Unable to enumerate runtime assets: $assetPath")
        if(children.isNotEmpty()) {
            makeDirectory(destination)
            children.forEach { name ->
                val child = checkedChild(destination, name)
                copyTree(assets, "$assetPath/$name", child)
            }
            return
        }
        val name = destination.name.lowercase(Locale.ROOT)
        if(name.endsWith(".so") || name.contains(".so.")) {
            throw IOException("Native libraries must be installed in nativeLibraryDir, not runtime assets: $assetPath")
        }
        makeDirectory(destination.parentFile ?: throw IOException("Runtime file has no parent"))
        assets.open(assetPath).buffered().use { input ->
            input.mark(4)
            val signature = IntArray(4) { input.read() }
            input.reset()
            if(signature.contentEquals(intArrayOf(0x7f, 0x45, 0x4c, 0x46))) {
                throw IOException("Executable ELF content is not permitted in runtime data assets: $assetPath")
            }
            destination.outputStream().buffered().use { output -> input.copyTo(output) }
        }
    }

    private fun checkedChild(parent: File, name: String): File {
        if(name.isEmpty() || name == "." || name == ".." || name.any { it == '/' || it == '\\' || it.code < 0x20 }) {
            throw IOException("Invalid runtime asset path component")
        }
        val directory = parent.canonicalFile
        val child = File(directory, name).canonicalFile
        if(child.parentFile != directory) throw IOException("Runtime asset path escapes its private directory")
        return child
    }

    private fun makeDirectory(directory: File) {
        if(!directory.isDirectory && !directory.mkdirs()) throw IOException("Unable to create private runtime directory")
    }
}
