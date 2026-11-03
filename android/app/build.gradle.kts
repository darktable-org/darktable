// SPDX-License-Identifier: GPL-3.0-or-later
import org.gradle.api.DefaultTask
import org.gradle.api.file.DirectoryProperty
import org.gradle.api.file.FileSystemOperations
import org.gradle.api.file.RegularFileProperty
import org.gradle.api.tasks.InputFile
import org.gradle.api.tasks.InputDirectory
import org.gradle.api.tasks.Input
import org.gradle.api.tasks.Internal
import org.gradle.api.tasks.OutputDirectory
import org.gradle.api.tasks.PathSensitive
import org.gradle.api.tasks.PathSensitivity
import org.gradle.api.tasks.TaskAction
import org.gradle.api.tasks.testing.Test
import org.gradle.api.provider.Property
import org.gradle.process.ExecOperations
import java.io.File
import javax.inject.Inject
import com.android.build.api.artifact.SingleArtifact
import com.android.build.api.variant.BuiltArtifactsLoader

abstract class PrepareMobileAssetsTask @Inject constructor(
    private val fileSystemOperations: FileSystemOperations,
) : DefaultTask() {
    @get:InputFile
    abstract val camerasXml: RegularFileProperty

    @get:InputFile
    abstract val noiseProfilesJson: RegularFileProperty

    @get:InputFile
    abstract val whiteBalancePresetsJson: RegularFileProperty

    @get:OutputDirectory
    abstract val outputDirectory: DirectoryProperty

    @TaskAction
    fun generate() {
        fileSystemOperations.sync {
            from(camerasXml)
            from(noiseProfilesJson)
            from(whiteBalancePresetsJson)
            into(outputDirectory)
        }
    }
}

abstract class PreparePrebuiltRuntimeTask @Inject constructor(
    private val execOperations: ExecOperations,
    private val fileSystemOperations: FileSystemOperations,
) : DefaultTask() {
    @get:InputDirectory
    @get:PathSensitive(PathSensitivity.RELATIVE)
    abstract val stageDirectory: DirectoryProperty

    @get:InputFile
    @get:PathSensitive(PathSensitivity.NONE)
    abstract val moduleManifest: RegularFileProperty

    @get:InputFile
    @get:PathSensitive(PathSensitivity.NONE)
    abstract val verifierScript: RegularFileProperty

    @get:InputFile
    @get:PathSensitive(PathSensitivity.NONE)
    abstract val elfParserScript: RegularFileProperty

    @get:Input
    abstract val pythonExecutable: Property<String>

    @get:OutputDirectory
    abstract val outputDirectory: DirectoryProperty

    // AGP assigns the actual generated-source paths to these properties. They
    // must be task outputs so its source providers retain the producer dependency.
    @get:OutputDirectory
    abstract val assetsDirectory: DirectoryProperty

    @get:OutputDirectory
    abstract val jniLibsDirectory: DirectoryProperty

    @TaskAction
    fun prepare() {
        execOperations.exec {
            commandLine(pythonExecutable.get(), verifierScript.get().asFile.absolutePath,
                "--stage", stageDirectory.get().asFile.absolutePath,
                "--modules", moduleManifest.get().asFile.absolutePath,
                "--output", outputDirectory.get().asFile.absolutePath)
        }
        fileSystemOperations.sync {
            from(outputDirectory.dir("assets"))
            into(assetsDirectory)
        }
        fileSystemOperations.sync {
            from(outputDirectory.dir("jniLibs"))
            into(jniLibsDirectory)
        }
        // Verify the directories AGP will package, after copying, against the
        // same manifest. The verified staging root is not itself a source set.
        execOperations.exec {
            commandLine(pythonExecutable.get(), verifierScript.get().asFile.absolutePath,
                "--stage", outputDirectory.get().asFile.absolutePath,
                "--packaged-assets", assetsDirectory.get().asFile.absolutePath,
                "--packaged-jni", jniLibsDirectory.get().asFile.absolutePath)
        }
    }
}

abstract class VerifyPrebuiltRuntimeApkTask @Inject constructor(
    private val execOperations: ExecOperations,
) : DefaultTask() {
    @get:InputDirectory
    @get:PathSensitive(PathSensitivity.RELATIVE)
    abstract val apkDirectory: DirectoryProperty

    @get:InputDirectory
    @get:PathSensitive(PathSensitivity.RELATIVE)
    abstract val stageDirectory: DirectoryProperty

    @get:InputFile
    @get:PathSensitive(PathSensitivity.NONE)
    abstract val moduleManifest: RegularFileProperty

    @get:InputFile
    @get:PathSensitive(PathSensitivity.NONE)
    abstract val verifierScript: RegularFileProperty

    @get:InputFile
    @get:PathSensitive(PathSensitivity.NONE)
    abstract val elfParserScript: RegularFileProperty

    @get:Input
    abstract val pythonExecutable: Property<String>

    @get:Internal
    abstract val builtArtifactsLoader: Property<BuiltArtifactsLoader>

    @TaskAction
    fun verify() {
        val artifacts = builtArtifactsLoader.get().load(apkDirectory.get())
            ?: throw GradleException("Cannot read the final APK output metadata")
        if(artifacts.elements.isEmpty()) throw GradleException("No final APK was produced for runtime verification")
        artifacts.elements.forEach { artifact ->
            execOperations.exec {
                commandLine(pythonExecutable.get(), verifierScript.get().asFile.absolutePath,
                    "--stage", stageDirectory.get().asFile.absolutePath,
                    "--modules", moduleManifest.get().asFile.absolutePath,
                    "--apk", artifact.outputFile)
            }
        }
    }
}

plugins { id("com.android.application"); id("org.jetbrains.kotlin.android"); id("org.jetbrains.kotlin.plugin.compose") }

val persistentSigningConfigured = listOf(
    "ANDROID_KEYSTORE_PATH",
    "ANDROID_KEYSTORE_PASSWORD",
    "ANDROID_KEY_ALIAS",
    "ANDROID_KEY_PASSWORD",
).all { providers.environmentVariable(it).isPresent }

val repositoryRoot = rootProject.projectDir.parentFile
val prebuiltStage = providers.gradleProperty("darktableRuntimeStage").orNull?.let { value ->
    val directory = File(value)
    if(value.isBlank() || !directory.isAbsolute || !directory.isDirectory) {
        throw GradleException("darktableRuntimeStage must be an absolute path to an existing complete Android runtime stage")
    }
    if(!directory.resolve("manifest.json").isFile) {
        throw GradleException("darktableRuntimeStage has no manifest.json; use stage-android-runtime.py output, not an install prefix or dependency probe")
    }
    directory
}
val prebuiltModules = providers.gradleProperty("darktableRuntimeModules").orNull?.let { value ->
    if(prebuiltStage == null) throw GradleException("darktableRuntimeModules requires darktableRuntimeStage")
    File(value).also {
        if(!it.isAbsolute || !it.isFile) throw GradleException("darktableRuntimeModules must be an absolute path to the original configured engine module manifest")
    }
} ?: prebuiltStage?.resolve("android-runtime-libraries.txt")
if(prebuiltModules != null && !prebuiltModules.isFile) {
    throw GradleException("The Android runtime stage has no configured module manifest; recreate it with the current staging tool or supply darktableRuntimeModules")
}
val vcpkgTag = "2025.06.13"
val vcpkgRoot = repositoryRoot.resolve(".vcpkg/$vcpkgTag")
val vcpkgInstalled = repositoryRoot.resolve(".vcpkg/installed")
val pinnedNdk = androidComponents.sdkComponents.ndkDirectory.map { it.asFile }
val prepareNativeDependencies = if(prebuiltStage == null) tasks.register<Exec>("prepareNativeDependencies") {
    inputs.files(
        repositoryRoot.resolve("mobile/dependencies/vcpkg.json"),
        repositoryRoot.resolve("mobile/dependencies/build-android-dependencies.sh"),
        repositoryRoot.resolve("mobile/dependencies/triplets/arm64-android.cmake"),
        repositoryRoot.resolve("mobile/dependencies/validate-android-triplet.cmake"),
    )
    outputs.dir(vcpkgInstalled.resolve("arm64-android"))
    environment("ANDROID_NDK_HOME", pinnedNdk.get().absolutePath)
    commandLine(repositoryRoot.resolve("mobile/dependencies/build-android-dependencies.sh"))
} else null
android {
    namespace = "org.example.darktableandroid"
    compileSdk = 35
    ndkVersion = "27.2.12479018"
    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
    defaultConfig {
        applicationId = "org.example.darktableandroid"
        minSdk = 26
        targetSdk = 35
        versionCode = 1
        versionName = "0.1.0"
        testInstrumentationRunner = "androidx.test.runner.AndroidJUnitRunner"
        ndk { abiFilters += "arm64-v8a" }
        if(prebuiltStage == null) externalNativeBuild { cmake {
            arguments += listOf(
                "-DANDROID_ABI=arm64-v8a",
                "-DANDROID_PLATFORM=android-26",
                "-DCMAKE_ANDROID_ARCH_ABI=arm64-v8a",
                "-DCMAKE_ANDROID_ARCH=aarch64",
                "-DANDROID_STL=c++_shared",
                "-DANDROID_SUPPORT_FLEXIBLE_PAGE_SIZES=ON",
                "-DCMAKE_TOOLCHAIN_FILE=${vcpkgRoot.resolve("scripts/buildsystems/vcpkg.cmake")}",
                "-DVCPKG_CHAINLOAD_TOOLCHAIN_FILE=${pinnedNdk.get().resolve("build/cmake/android.toolchain.cmake")}",
                "-DVCPKG_TARGET_TRIPLET=arm64-android",
                "-DVCPKG_INSTALLED_DIR=$vcpkgInstalled",
                "-DVCPKG_MANIFEST_MODE=OFF",
                // The full upstream graph currently requires desktop GTK and
                // plugin dependencies which are not part of the Android bundle.
                "-DDT_MOBILE_WITH_DARKTABLE_ENGINE=OFF",
            )
        } }
    }
    signingConfigs {
        create("productionRelease") {
            val path = providers.environmentVariable("ANDROID_KEYSTORE_PATH").orNull
            if(path != null) storeFile = file(path)
            storePassword = providers.environmentVariable("ANDROID_KEYSTORE_PASSWORD").orNull
            keyAlias = providers.environmentVariable("ANDROID_KEY_ALIAS").orNull
            keyPassword = providers.environmentVariable("ANDROID_KEY_PASSWORD").orNull
        }
    }
    flavorDimensions += "distribution"
    productFlavors {
        create("dev") {
            dimension = "distribution"; applicationIdSuffix = ".dev"; versionNameSuffix = "-dev"
            resValue("string", "app_name", "Mobile RAW Editor Dev (Unofficial)")
        }
        create("production") {
            dimension = "distribution"; resValue("string", "app_name", "Mobile RAW Editor (Unofficial)")
        }
    }
    buildTypes {
        debug { isDebuggable = true }
        release {
            isMinifyEnabled = false // TODO: enable after JNI/Compose keep rules are verified.
            isShrinkResources = false
            ndk { debugSymbolLevel = "SYMBOL_TABLE" }
            proguardFiles(getDefaultProguardFile("proguard-android-optimize.txt"), "proguard-rules.pro")
        }
    }
    signingConfigs.getByName("productionRelease").let { productionSigning ->
        androidComponents.beforeVariants(androidComponents.selector().withBuildType("release")) { builder ->
            if(builder.productFlavors.any { it.second == "production" } && System.getenv("ANDROID_KEYSTORE_PATH") != null) {
                builder.enable = true
            }
        }
        if(System.getenv("ANDROID_KEYSTORE_PATH") != null) productFlavors.getByName("production").signingConfig = productionSigning
    }
    // Local and pull-request builds use the debug key; development releases use the persistent key when configured.
    productFlavors.getByName("dev").signingConfig = if(persistentSigningConfigured) {
        signingConfigs.getByName("productionRelease")
    } else {
        signingConfigs.getByName("debug")
    }
    // Keep Android on the deliberately small, Android-compatible graph.  The
    // upstream root graph also configures desktop-only dependencies.
    if(prebuiltStage == null) externalNativeBuild { cmake { path = file("../../mobile/CMakeLists.txt"); version = "3.22.1" } }
    buildFeatures { compose = true; buildConfig = true }
    testOptions { unitTests.isIncludeAndroidResources = true }
    packaging {
        jniLibs {
            // The prebuilt manifest records exact, previously verified bytes.
            // AGP stripping must not silently replace them after verification.
            keepDebugSymbols += if(prebuiltStage != null) "**/*.so" else "**/libdt_mobile.so"
            // The desktop plugin registry enumerates nativeLibraryDir. Ask
            // Android's installer to extract code into that trusted directory
            // so processing modules can be discovered by their category prefix.
            useLegacyPackaging = true
        }
    }
}

androidComponents.onVariants { variant ->
    val variantName = variant.name.replaceFirstChar { it.uppercase() }
    if(prebuiltStage != null) {
        val prepareRuntime = tasks.register<PreparePrebuiltRuntimeTask>("prepare${variantName}PrebuiltRuntime") {
            stageDirectory.set(prebuiltStage)
            moduleManifest.set(checkNotNull(prebuiltModules))
            verifierScript.set(repositoryRoot.resolve("mobile/dependencies/verify-android-runtime-stage.py"))
            elfParserScript.set(repositoryRoot.resolve("mobile/dependencies/stage-android-runtime.py"))
            pythonExecutable.set(providers.gradleProperty("darktablePython").orElse("python3"))
            outputDirectory.set(layout.buildDirectory.dir("generated/prebuiltRuntime/${variant.name}"))
            assetsDirectory.set(outputDirectory.dir("assets"))
            jniLibsDirectory.set(outputDirectory.dir("jniLibs"))
        }
        variant.sources.assets?.addGeneratedSourceDirectory(prepareRuntime, PreparePrebuiltRuntimeTask::assetsDirectory)
        variant.sources.jniLibs?.addGeneratedSourceDirectory(prepareRuntime, PreparePrebuiltRuntimeTask::jniLibsDirectory)
        val verifyRuntimeApk = tasks.register<VerifyPrebuiltRuntimeApkTask>("verify${variantName}PrebuiltRuntimeApk") {
            apkDirectory.set(variant.artifacts.get(SingleArtifact.APK))
            builtArtifactsLoader.set(variant.artifacts.getBuiltArtifactsLoader())
            stageDirectory.set(prebuiltStage)
            moduleManifest.set(checkNotNull(prebuiltModules))
            verifierScript.set(repositoryRoot.resolve("mobile/dependencies/verify-android-runtime-stage.py"))
            elfParserScript.set(repositoryRoot.resolve("mobile/dependencies/stage-android-runtime.py"))
            pythonExecutable.set(providers.gradleProperty("darktablePython").orElse("python3"))
        }
        tasks.matching { it.name == "assemble$variantName" }.configureEach {
            dependsOn(verifyRuntimeApk)
        }
    } else {
        val prepareMobileAssets = tasks.register<PrepareMobileAssetsTask>(
            "prepare${variantName}MobileAssets",
        ) {
            camerasXml.set(
                layout.projectDirectory.file("../../src/external/rawspeed/data/cameras.xml"),
            )
            noiseProfilesJson.set(layout.projectDirectory.file("../../data/noiseprofiles.json"))
            whiteBalancePresetsJson.set(layout.projectDirectory.file("../../data/wb_presets.json"))
            outputDirectory.set(layout.buildDirectory.dir("generated/mobileAssets/${variant.name}"))
        }
        variant.sources.assets?.addGeneratedSourceDirectory(
            prepareMobileAssets,
            PrepareMobileAssetsTask::outputDirectory,
        )
    }
}

tasks.configureEach {
    if(prepareNativeDependencies != null && (name.startsWith("configureCMake") || name.startsWith("buildCMake"))) {
        dependsOn(prepareNativeDependencies)
    }
}

kotlin { jvmToolchain(17) }

// Resolve the host test SDK through Gradle, rather than Robolectric's separate
// downloader and user-home lock. Keep this SDK 35/i7 pin aligned with
// Robolectric 4.14.1 and WorkspaceTabNavigationTest's @Config.
val robolectricSdk by configurations.creating {
    isCanBeConsumed = false
    isTransitive = false
}
val prepareRobolectricSdk = tasks.register<Sync>("prepareRobolectricSdk") {
    from(robolectricSdk)
    into(layout.buildDirectory.dir("robolectric-sdk"))
}
tasks.withType<Test>().configureEach {
    dependsOn(prepareRobolectricSdk)
    systemProperty("robolectric.offline", "true")
    systemProperty("robolectric.dependency.dir", layout.buildDirectory.dir("robolectric-sdk").get().asFile.absolutePath)
}

dependencies {
    // Final APK verification pins the original ARM64 native path library bytes.
    implementation("androidx.graphics:graphics-path") { version { strictly("1.0.1") } }
    implementation(platform("androidx.compose:compose-bom:2025.04.01"))
    implementation("androidx.activity:activity-compose:1.10.1")
    implementation("androidx.compose.material3:material3")
    implementation("androidx.compose.ui:ui")
    implementation("androidx.lifecycle:lifecycle-viewmodel-compose:2.8.7")
    implementation("androidx.lifecycle:lifecycle-viewmodel-ktx:2.8.7")
    implementation("androidx.lifecycle:lifecycle-runtime-compose:2.8.7")
    implementation("org.jetbrains.kotlinx:kotlinx-coroutines-android:1.10.2")
    testImplementation("junit:junit:4.13.2")
    testImplementation("org.json:json:20240303")
    testImplementation("org.robolectric:robolectric:4.14.1")
    testImplementation(platform("androidx.compose:compose-bom:2025.04.01"))
    testImplementation("androidx.compose.ui:ui-test-junit4")
    robolectricSdk("org.robolectric:android-all-instrumented:15-robolectric-12650502-i7")
    androidTestImplementation("androidx.test:runner:1.6.2")
    androidTestImplementation("androidx.test.ext:junit:1.2.1")
    androidTestImplementation(platform("androidx.compose:compose-bom:2025.04.01"))
    androidTestImplementation("androidx.compose.ui:ui-test-junit4")
    debugImplementation("androidx.compose.ui:ui-test-manifest")
}
