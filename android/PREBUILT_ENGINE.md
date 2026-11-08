<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
# Optional prebuilt Android engine

The default Android build still compiles the dependency-only native bridge with
the processing engine disabled. A complete native Android engine can instead be
supplied explicitly as a verified prebuilt stage. This does not make an
incomplete dependency probe into an editor or establish device support.

The configured ARM64/API 26 engine has now been built, installed and staged.
An experimental engine-enabled development APK has passed assembly and final
runtime byte/hash verification. A matching main/test APK pair passed all 41
Fusion GPR JNI phases on an official ARM64/API 28 guest with host single-step
emulation and fixed virtual timing, including native loading, default-instance
editing/undo/redo and 3104×3000 PNG export/reopen. Physical devices and all-format
coverage remain unverified. The dated
[port status](../mobile/dependencies/PORTING_STATUS.md) records the exact build
and validation boundary.

## Downloading an experimental APK

The `Android engine APK downloads` workflow publishes raw APK assets to a
source-specific GitHub prerelease named `android-engine-<12-character-source-SHA>`.
Download `darktable-android-engine-arm64.apk` from that release's **Assets**;
it installs directly without opening an Actions artifact ZIP. The separately
named instrumentation APK is for native test execution, not the editor app.
The checksum and validation notes remain attached beside the APKs.

The publisher first downloads the full-engine Actions artifact back from GitHub,
requires its main/test APKs, verifies source and SHA-256 records, and checks APK
ZIP integrity and engine contents. It does not publish the dependency-only APK
from the normal Android CI. The main engine workflow calls this publisher after
successful packaging; its standalone push/manual path can recover an existing
successful engine run without rebuilding the native dependency graph.

A Release download does not establish full desktop parity or physical-device
support. The default Gradle engine and existing `dev-latest` release are separate.

First finish the ARM64/API 26 [engine build](../mobile/dependencies/ENGINE_BOOTSTRAP.md),
install it, and run the current [staging tool](../mobile/dependencies/STAGING.md)
with `--ndk`. Use one matching engine version/configuration for the core, JNI,
plugins and complete runtime data. The resulting stage must include:

```text
manifest.json
android-runtime-libraries.txt
assets/darktable-runtime/data/...
assets/darktable-runtime/locale/...
assets/darktable-runtime/lensfun/version_1/...
jniLibs/arm64-v8a/*.so
```

For a smaller installation APK, install the completed native build into a
fresh prefix with `cmake --install /path/to/build --prefix /fresh/prefix --strip`,
then stage that prefix. Keep the original build libraries for symbolized crash
diagnostics. Stripping is an installation step; staging and Gradle still retain
the installed bytes exactly. When replacing a much larger native runtime in an
existing Android build directory, clear the generated APK/package incremental
state before repackaging so unused ZIP space is not retained. A fresh CI build
does not have that incremental archive history.

Build using its absolute path:

```sh
./gradlew :app:assembleDevDebug \
  -PdarktableRuntimeStage=/absolute/path/to/android-runtime-stage
```

This branch disables the standalone CMake build and its dependency-preparation
task. It uses only the stage's native libraries and complete runtime assets,
avoiding a collision with the dependency-only `libdt_mobile.so`. The ABI remains
`arm64-v8a`. Existing installer extraction into `nativeLibraryDir` remains
enabled for native plugin discovery.

Each variant registers one generated-source producer for both assets and JNI
libraries. Python 3.10+ verifies the input stage and a fresh copied generation
under `app/build/generated/prebuiltRuntime/<variant>/`. The task then copies
those verified sources to AGP's generated assets/JNI directories and verifies
the actual files AGP will package against the same manifest:

* SHA-256 values and exact native/data file lists, including no unrecorded files.
* The original configured module list against `configuredLibraries` in JSON and
  every packaged core/JNI/processing/format/storage module.
* AArch64 ELF64 libraries, SONAME/search paths and the complete non-system
  `DT_NEEDED` closure. JNI must depend on `libdarktable.so`; a dependency-only
  JNI probe is rejected.
* API 26 system-stub verification recorded by the staging producer, required
  nonempty runtime data, original translation catalogs and Lensfun calibration XML.
* Native code only in the flat JNI directory, with no ELF or `.so` files in data
  assets, symlinks, extra ABIs or unexpected asset roots.

The original configured list normally travels in the stage. If it is stored
separately, provide its absolute path with
`-PdarktableRuntimeModules=/absolute/path/android-runtime-libraries.txt`. It must
exactly match the JSON list; it does not repair an old or incomplete stage.
Use `-PdarktablePython=/absolute/path/to/python3` when `python3` is not on PATH.
The verifier can also run independently:

```sh
python3 ../mobile/dependencies/verify-android-runtime-stage.py \
  --stage /absolute/path/to/android-runtime-stage
```

Prebuilt builds retain every native library's exact staged bytes instead of
letting AGP strip them. Each `assemble<Variant>` also runs
`verify<Variant>PrebuiltRuntimeApk` against the final APK reported by AGP. This
checks the archive after merging, packaging and signing, including the complete
native/data file lists, hashes, configured modules and dependency closure. It
rejects extra ABIs, missing dynamically loaded plugins, changed libraries/data,
duplicate ZIP entries and ambiguous runtime paths. Keeping symbols can increase
APK size; produce stripped libraries before staging if a smaller artifact is
needed, then recreate the stage and its hashes from those exact inputs.

Compose also contributes `libandroidx.graphics.path.so` from the strictly pinned
`androidx.graphics:graphics-path:1.0.1` dependency. It remains separate from the
engine stage. APK verification accepts only its original ARM64 SHA-256
`41e9a793c43a0f4fddb19e33f346bace464f30f888ba7b9eaf96294ea115bfb6`, validates
its ELF/SONAME/search paths and dependency closure, and reports it separately as
`appNativeLibraries`. Unknown or changed application native libraries still fail.
The official Google Maven AAR has SHA-256
`8ca4032b6d79b351f0b59ad4b580eddbb9423e1652f7c958830687f1eee2ec03`.
Stage and generated-source verification retain their exact engine-only file list.

The same final-artifact check can run on a copied APK or after an external
signing step:

```sh
python3 ../mobile/dependencies/verify-android-runtime-stage.py \
  --stage /absolute/path/to/android-runtime-stage \
  --apk app/build/outputs/apk/dev/debug/app-dev-debug.apk
```

The verifier reports the APK SHA-256 for the audited artifact. It checks runtime
contents, not the APK signature, installability or Android execution. Use the SDK's
`apksigner verify --verbose` for signature verification. Install that same audited
APK on an ARM64 Android API 26+ device and exercise open/edit/undo/export/cancel/close
with original RAW and GPR inputs; compare full-resolution outputs with the matching
desktop build. An x86_64-only emulator without an ARM64 native bridge cannot
load this package. An emulator must provide ARM64 execution (or separately
validated ARM64 translation), and API 26 linker
compatibility must also be checked on a matching runtime. Host ELF parsing or
packaging tests do not replace those checks.

Direct asset/native merge tasks use the same verified producer, rather than
copying unchecked source directories. Generated files are build outputs; do not
edit them or place them in source control. Remove the property to return to the
default dependency-only route. A missing/incomplete stage fails explicitly and
does not fall back to an engine-disabled APK.

The real stage contains the configured core, JNI, modules, matching runtime data
and checked shared-library closure. The experimental APK's final runtime
contents have passed verification; native loader behavior and RAW/GPR
edit/export/cancel on real devices remain unverified. The configured manifest
covers only the selected native build; optional codecs and
unported editing controls remain separate work. The opt-in
[engine workflow](../.github/workflows/android-engine.yml) records this route and
also runs for native/runtime changes on `feature/android-raw-editor`.
[Run 40722614711](https://github.com/togekk39/darktable-android/actions/runs/40722614711)
at `90ce43520f` passed the complete native build/staging/packaging checks and
uploaded the signed engine APK and paired instrumentation APK as artifact
`12833671749`. That CI job did not install or run the APK; the local API 28
execution above is separate evidence. See the
[status notes](../mobile/dependencies/PORTING_STATUS.md) for the recorded
configuration and artifact identity.

Offline producer/verifier regression tests use synthetic ELF headers and ZIP
fixtures, without loading or simulating the processing engine:

```sh
python3 -B -m unittest discover -s ../mobile/dependencies/tests -v
```
