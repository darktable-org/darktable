<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
# Android native engine execution test

## Expanded editor controls: current source validation

The current editor exposes the reviewed scalar and fixed-array parameters,
curves, blending and parametric masks, native path/brush forms and node editors,
retouch/spot forms, liquify point warps, native-input-aware crop aspect drafts,
text fields and file imports described in
[control coverage](../mobile/CONTROL_PARITY.md). Runtime metadata depends on
the image, module instances and native capabilities; source-policy field counts
are not counts of independently rendered Android controls.

The current validation includes:

- The coordinated Android check passed production and instrumentation Kotlin
  compilation, 92 JVM tests in 23 classes (zero failures or skips), and
  `lintDevDebug`. Additional instrumentation-source checks remain separate from
  actually running those component tests on Android.
- The final expanded source passed all seven host native CTest cases in
  161.26 seconds, including the original-engine regression in 161.16 seconds.
  Its RGB fixture traversed 1,207
  advertised ordinary parameters, including live-instance and blending fields;
  this runtime count is distinct from the source-policy field counts. The run
  includes minimum-node and invalid-topology checks for paths and brushes.
- The preceding `9e749eeb6e` snapshot also passed production JNI compilation with the pinned
  AArch64/API 26 NDK, plus a host
  JNI test using the original engine: four Unicode round trips and five invalid
  UTF-16/NUL rejections. The host JVM uses `-Xint` because desktop darktable's
  signal handler conflicts with HotSpot JIT; Android already omits that handler.
- The generated-C enum and aligned-double introspection regressions.

The native engine regression uses the original processing modules with a small
RGB fixture. It traverses every advertised ordinary parameter, sets allowed
current values through the public API and verifies exact metadata restoration
after undo. Imported values outside the reviewed write domain must reject
without changing metadata or history. Separate comparisons exercise actual
pixel changes and exact undo/redo for representative expanded modules, native
blending/mask/liquify controls, and imported LUT/overlay/raster resources. Native
repair forms additionally have render, reset and metadata/history checks.
The close/reopen path explicitly exports a PNG using the default color profile
to guard against retaining unloaded colorin/colorout modules across runtimes.
The fixture's runtime metadata lists 85 visible RGB modules. Each has specific
numeric, curve, text or dedicated controls except legacy `colortransfer`, whose
original desktop editor also retains only enable/disable. RAW-only modules are
omitted for that RGB fixture.

The expanded regression adds real parametric threshold pixels and undo/redo,
inclusive-mode neutral channels, boost-coordinate preservation, invalid handle
ordering and unsupported-channel rejection. Path/brush checks exercise original
form creation, precise nodes/handles, insertion/deletion, group combinations,
native rendering and history; special repair consumers follow their original
supported shape families. Crop geometry checks use preceding pixel-aspect and
orientation edits to distinguish the crop input ROI from its processed output.
Android JVM tests separately cover threshold bounds/graphs, mask address and
node selection, crop aspect fitting and optional native dimension metadata.

The fixed workspace layout adds six JVM category/selection checks and ten
`EditorWorkspaceTest` component tests. The component sources compile with the
production editor and cover the split panes, explicit power switch, stable
instance selection, scrolling, Settings/license access, busy actions, saved tab
restoration and offscreen history/draft restoration. They use bitmap/callback
fixtures and have not been executed on an Android device in this validation.
The layout revision does not change the native processing code.

These checks do not establish execution of the new Compose instrumentation
tests on Android, complete physical-device picker/edit/export behavior, every
parameter's pixel effect, or new RAW/GPR coverage. Build the matching
engine-enabled APK and execute the instrumentation classes on an ARM64 Android
device for that evidence. Historical Android execution results below apply to
their named source snapshots only.

## Sliders and expanded desktop controls

`ParameterControlTest` exercises production numeric drag/release, native-default
reset callbacks, enum choice menus, boolean switches and busy-state disabling.
`SpecialSliderControlsTest` exercises the white-balance Kelvin/gain sliders and
crop sliders, ensuring the grouped values remain drafts until Apply. These
component tests use callback fixtures; they do not establish native pixel edits.
Run them together with the existing three `ModuleControlsTest` methods:

```sh
"$ADB_BIN" -s "$TEST_SERIAL" shell am instrument -w -r \
  -e class org.example.darktableandroid.ModuleControlsTest,org.example.darktableandroid.ParameterControlTest,org.example.darktableandroid.SpecialSliderControlsTest \
  "$APP_PACKAGE.test/androidx.test.runner.AndroidJUnitRunner"
```

`NativeAdditionalControlsTest` separately uses the real JNI engine with a
generated 32x24 RGB PNG. It checks the screenshot's tone-mapping contrast/spatial
extent, vignette pixel changes and strict undo/redo, native enums and booleans,
exact nested paths, narrower safe domains, desktop slider extents, original
default resets and unchanged source hashes. Its eleven preview calls include
an explicit readiness pass and a byte-identical repeatability check. Other new
controls use native metadata/setter/history round trips; their pixel algorithms
are covered separately by the host fixture and are not all rendered here.

```sh
"$ADB_BIN" -s "$TEST_SERIAL" shell am instrument -w -r \
  -e class org.example.darktableandroid.nativecore.NativeAdditionalControlsTest \
  -e requireAdditionalControls true \
  "$APP_PACKAGE.test/androidx.test.runner.AndroidJUnitRunner"
"$ADB_BIN" -s "$TEST_SERIAL" exec-out run-as "$APP_PACKAGE" \
  cat files/additional-controls/result.json > android-additional-controls-result.json
```

Require the runner's successful counts and a JSON report whose `status` is
`passed` and `phase` is `complete`. Compiling these tests is not execution
evidence. This historical test covers the earlier 103-field review boundary;
see [control coverage](../mobile/CONTROL_PARITY.md) for the current scope and
outstanding desktop controls.

The source content committed locally as `ca5a4875d4` passed all 12 production
Compose component tests and all 50 JNI phases on the official ARM64/API 28
guest. The native test made 11 previews, found 103 original defaults and 15
desktop soft ranges, and preserved source SHA-256
`c6ef76f2260d8d96a8882e8687599dc433b2e76719450e1196c1f76920bcb5ec`
before edits, after edits and after close. Both runner counts and the completed
JSON report passed. Host single-step emulation with `icount` shift 0 and sleep
enabled was used; virtual durations do not measure phone performance.

The actual tested main APK is 40,464,814 bytes, SHA-256
`11f267a9ce621b38876ff0459a190d9edb3161c33a09a7a29bb13c9cc7351632`;
the paired test APK is 1,183,173 bytes, SHA-256
`285eca0a4948f8c26c2a043b3bf23af14554bb6ddd0c2f0b4a591d29843c873a`.
Native version metadata was built before the local commit as
`41753a3006-dirty`; the source content and recorded native source hashes match
the commit. Final runtime bytes, all 111 native libraries' 16 KiB load
alignment, v2 signatures and compact APK ZIP layout passed. Curated evidence
is under `work/deliverables/tool-sliders-validation/`, with the actual parsed
report at `work/deliverables/android-additional-controls-result.json`.
These are component and small RGB native tests, separate from a complete
physical-device picker/edit/export workflow and broader RAW/GPR coverage.

## Parameter editor and color-control regression

`ModuleControlsTest` uses the production Compose module component with callback
fixtures. It tests explicit expansion, entering and applying signed exposure,
enabling a module to reveal its editor, collapse and the unavailable-controls
explanation. It does not simulate a native pixelpipe.

`NativeColorControlsTest` separately creates a 32x24 RGB PNG fixture and uses real
JNI plus the original `colisa` and `colorize` modules. Set
`-e requireColorControls true` so a missing engine is a failure. It verifies all
seven fields, native bounds, private-version rejection, native-to-Kotlin control
discovery and initial selection, pixel changes, exact undo/redo and source hashes.
The 19 preview calls include explicit priming and a strict repeatability check;
the existing first-render observation on a host PNG is tracked in PORTING_STATUS.

```sh
"$ADB_BIN" -s "$TEST_SERIAL" shell am instrument -w -r \
  -e class org.example.darktableandroid.ModuleControlsTest \
  "$APP_PACKAGE.test/androidx.test.runner.AndroidJUnitRunner"
"$ADB_BIN" -s "$TEST_SERIAL" shell am instrument -w -r \
  -e class org.example.darktableandroid.nativecore.NativeColorControlsTest \
  -e requireColorControls true \
  "$APP_PACKAGE.test/androidx.test.runner.AndroidJUnitRunner"
"$ADB_BIN" -s "$TEST_SERIAL" exec-out run-as "$APP_PACKAGE" \
  cat files/color-controls/result.json > android-color-controls-result.json
```

The pair built from the source content committed locally as `6283ef1858` passed
all three Compose tests and the 53-phase JNI test on the official ARM64/API 28
guest using host single-step emulation with fixed virtual timing. Main APK SHA-256
`1cb0f7e1e07149105149a31825cfbb9b7e34851c543790bfb36ce1458f19cdbf`;
test APK `87bbc1c9e0e208973eb9719960b70d3bb1df2c95cd16a0c38e24bd113a6f2e7b`.
Native version metadata was built before that commit as `d2ec88d415-dirty`;
the actual source content matches the committed change. This is a small RGB
fixture/component test, separate from RAW/GPR and complete app/physical-device
coverage. Require both the runner's successful test counts and a completed JSON
report with status `passed`.

## Fusion RAW/GPR execution

`NativeEngineSmokeTest` runs the installed Android JNI/core/modules through the
same public adapter used by the editor. It requires an ARM64 Android API 26+
device or an actual ARM64 Android emulator, a complete engine-enabled APK and an
original RAW/GPR fixture. The default dependency-only APK fails its capability
check. Building the test APK alone is not Android execution evidence.

Build both APKs from a complete verified stage, using the tools described in
[PREBUILT_ENGINE.md](PREBUILT_ENGINE.md):

```sh
android/gradlew --no-daemon -p android \
  :app:assembleDevDebug :app:assembleDevDebugAndroidTest \
  -PdarktableRuntimeStage=/absolute/path/to/real-runtime-stage
```

The test APK is
`android/app/build/outputs/apk/androidTest/dev/debug/app-dev-debug-androidTest.apk`.
It contains no RAW sample or native processing replacement. In the managed
development environment, the local Gradle init script instead redirects both
APKs to `work/android-build/_app/outputs/apk/` and supplies the private work
directory's debug signing key.

Copy a fixture into the target application's private files directory. This does
not require image/storage permissions, a document-picker mock or a gallery UI.
For example, `GPBK7066.GPR` from the pinned official GoPro archive has the
previously verified output dimensions `3104 x 3000`:

```sh
ADB_BIN="$ANDROID_SDK_ROOT/platform-tools/adb"
TEST_SERIAL=emulator-5554
APP_PACKAGE=org.example.darktableandroid.dev

"$ADB_BIN" -s "$TEST_SERIAL" install -r /path/to/app-dev-debug.apk
"$ADB_BIN" -s "$TEST_SERIAL" install -r /path/to/app-dev-debug-androidTest.apk
"$ADB_BIN" -s "$TEST_SERIAL" shell run-as "$APP_PACKAGE" mkdir -p files/engine-smoke
"$ADB_BIN" -s "$TEST_SERIAL" exec-in \
  "run-as $APP_PACKAGE sh -c 'cat > files/engine-smoke/input.GPR'" \
  < /path/to/original/GPBK7066.GPR
APP_DATA_DIR=$("$ADB_BIN" -s "$TEST_SERIAL" shell run-as "$APP_PACKAGE" pwd | tr -d '\r')

"$ADB_BIN" -s "$TEST_SERIAL" shell am instrument -w -r \
  -e class org.example.darktableandroid.nativecore.NativeEngineSmokeTest \
  -e inputFile "$APP_DATA_DIR/files/engine-smoke/input.GPR" \
  -e expectedWidth 3104 -e expectedHeight 3000 \
  -e timeout_msec 3600000 \
  "$APP_PACKAGE.test/androidx.test.runner.AndroidJUnitRunner"

"$ADB_BIN" -s "$TEST_SERIAL" exec-out run-as "$APP_PACKAGE" \
  cat files/engine-smoke/result.json > android-engine-result.json
python3 -c 'import json; r=json.load(open("android-engine-result.json")); assert r["status"] == "passed", r'
```

Use the device's actual serial and matching debug signatures. Fixture files,
APKs, keys, exports and result logs are generated inputs/outputs and must remain
outside source control. The fixture path must resolve inside the target app's
private files directory. No original input is overwritten.

The single execution test checks the real engine and VC-5 capability bits,
runtime extraction/configuration, original RAW open, actual preview pixels,
exposure changes, Kelvin/tint through the shared desktop calculation core,
pixel-exact undo/redo, new default exposure instances with independent settings,
topology undo/redo and retired-selector rejection, native full-size PNG export and Android PNG decoding,
source SHA-256 protection, and close/reopen with the original pixels restored.
The PNG check establishes full dimensions and decodability; it does not compare
that export against a second unedited full-resolution export. Preview edit
changes are compared separately.
The current test requires the native `canCreateInstance` capability and fails an
older engine instead of skipping those checks. Use a matching main/test APK pair.

`files/engine-smoke/result.json` records the device/API/ABI, current phase,
completed phase durations, preview hashes, original-source hashes and completed
export location/hash. The file is updated before each native operation; a
native crash can leave `status: running` at its last phase. A Java/JNI exception
records `status: failed`. Require both the instrumentation runner's successful
test result and `status: passed`; a shell command's exit status alone is not
sufficient. Successful exports remain under a unique private `run-*` directory
for inspection. The tag `NativeEngineSmoke` in logcat provides live phase markers.

Even a successful run covers one fixture on that Android runtime. It does not
establish all RAW/camera types, hardware/driver behavior, every desktop editing
tool, all optional codecs, UI interaction, lifecycle interruption or complete
desktop feature parity.

## XCF precision and independent pixel validation

`NativeXcfExportTest` separately exercises the original XCF writer through JNI.
Use a matching main/test APK pair built from an XCF-enabled runtime. This test
requires the original official Fusion `GPBK7066.GPR`, with SHA-256
`c03eccdc334dc5b35cf6385799b219f4c9a0bc18ccbe70fe433237d98f352247`.
The supplied path must resolve inside the target application's private files
directory; the preceding fixture-copy example can be reused.

```sh
"$ADB_BIN" -s "$TEST_SERIAL" shell am instrument -w -r \
  -e class org.example.darktableandroid.nativecore.NativeXcfExportTest \
  -e requireXcf true \
  -e inputFile "$APP_DATA_DIR/files/engine-smoke/input.GPR" \
  -e timeout_msec 3600000 \
  "$APP_PACKAGE.test/androidx.test.runner.AndroidJUnitRunner"

"$ADB_BIN" -s "$TEST_SERIAL" exec-out run-as "$APP_PACKAGE" \
  cat files/engine-xcf/result.json > android-xcf-result.json
python3 -c 'import json; r=json.load(open("android-xcf-result.json")); assert r["status"] == "passed" and r["phase"] == "complete" and len(r["exports"]) == 3, r'
```

`requireXcf=true` makes a missing real engine, VC-5 decoder or XCF module fail
the test. Without this argument, unavailable capabilities can produce an
explicit skipped result; a skipped result is not validation. Require both the
instrumentation runner's successful test result and the completed JSON report.

The test opens that original GPR, applies exposure through the desktop module,
and exports three XCF/TIFF pairs with the same settings: 8-bit unsigned,
16-bit unsigned and 32-bit float, sRGB output, source metadata excluded, and
both dimensions bounded to 128 pixels. It checks XCF v012, RGB dimensions and
the requested gamma precision codes (`150`, `250`, `650`). Source SHA-256 is
checked before opening, after all exports and after closing the native session.
The existing 41-phase smoke test remains a separate execution test.

`files/engine-xcf/result.json` is updated atomically before every named native
phase and records completed phases, device details, settings, source hashes and
each output's private path, byte count and SHA-256. Exports remain under a
unique `files/engine-xcf/run-*` directory. Pull each XCF and its `referenceTiff`
from those recorded paths using `exec-out run-as`, then verify their hashes.
The `NativeXcfExport` logcat tag identifies live phases.

The instrumentation checks container headers; it does not decode XCF pixels.
Independent host GIMP loading and pixel comparison against each paired TIFF
must preserve the requested precision, especially 32-bit float. Keep that
separate evidence with the report. Successful XCF export does not establish XCF
import, mask/channel export or complete format parity.

The pair built from local source `d866419cb5` passed all 18 phases on the official
ARM64/API 28 guest with host single-step emulation and fixed virtual timing.
Main APK SHA-256 is `8a564200bb31a006d2b93b685d3635841c379cc5913484336a838575492b4d90`;
test APK is `a1a9b993d24d9750995c7186984face0a33662802c4d766b647ef86a0792c515`.
GIMP 3.0.4 independently loaded all three pulled 128x123 XCF/TIFF pairs and
compared complete native RGB bytes without reducing precision. Pixels and
embedded sRGB ICC profiles matched exactly at all three precisions; float values
were finite. Source hashes matched before/after exports and after close. This
is one fixture under the recorded emulator mode, not physical-device or
performance evidence. The work-only reports are `work/xcf-validation/android-result.json`
and `android-gimp-report.json` in that directory.

## Optional ARM64 emulator instruction diagnostic

The standalone sources in
[`mobile/platform/android/tests`](../mobile/platform/android/tests) reproduce an
ARM64 vector instruction sequence independently of darktable. They are not
linked into the APK or engine and are not RAW processing tests. The assembly
preserves the contiguous sequence from an NDK 27.2 vectorized `size_t` loop; the
C checker is compiled at `-O0`, checks allocation canaries and verifies all
65,536 resulting values. The executable uses neither OpenMP nor libm.

Build it with the pinned NDK into an explicitly supplied fresh directory, then
run it on the Android runtime being investigated:

```sh
PROBE_BUILD="$PWD/work/vector-sequence-probe"
mobile/platform/android/tests/build-vector-sequence.sh \
  --ndk "$ANDROID_NDK_HOME" --output "$PROBE_BUILD"
"$ADB_BIN" -s "$TEST_SERIAL" push "$PROBE_BUILD/vector-sequence" \
  /data/local/tmp/dt-vector-sequence
"$ADB_BIN" -s "$TEST_SERIAL" shell chmod 700 /data/local/tmp/dt-vector-sequence
"$ADB_BIN" -s "$TEST_SERIAL" shell /data/local/tmp/dt-vector-sequence \
  > vector-sequence-result.txt
cat vector-sequence-result.txt
```

Require exit status zero and the final `all65536values=exact` marker. Capture
logcat/tombstone evidence on a native crash. Generated binaries and logs stay
outside source control; the build script rejects an existing output directory
and checks the ELF's shared-library dependencies.

On 2026-10-26, the original standalone assembly reproducer failed with
`SIGSEGV` on official emulator 30.8.4 build 7600983, running the unmodified
official ARM64 API 26 image. Official emulator 37.2.12 subsequently reproduced
the failure with that image and with the official AOSP API 28 ARM64 image.
QMP on emulator 37.2.12 reports QEMU `2.12.0-19097-g85fa07f04ef`, so those SDK
release numbers do not establish independent QEMU implementations.

On 2026-10-27 the same API 28 guest and original dynamic diagnostic binary
(SHA-256 `819f9b660db90649402c349199c2ca93697439cb78cd5a5580b93f27044a854c`)
failed in normal host mode, then passed after QMP/HMP `singlestep on`. The host
reported `singlestep: true`; the diagnostic returned exit 0 with
`canaries=preserved all65536values=exact`. This setting changes host emulation,
not the Android images, authentication, SELinux policy, engine algorithms or
production compiler flags. The API 28 framework also completed boot with
`sys.boot_completed=1` and SELinux enforcing. A later cold boot with fixed host
instruction timing (`-singlestep -icount shift=0,align=off,sleep=on`) passed the
earlier main/test pair's complete Fusion JNI edit/undo/full-size PNG export/
source-protection/reopen test. The newer test described above additionally
requires default-instance checks and also passed all 41 phases with main APK
`734cfbdafff4cbac0f69cd7fb00f6ff5e0d25f61dd0044d80ad441be4840d225`
and test APK `228022cacd52d795d5e36002d184883dfb1bb5594e04e16fb32545a50f289b20`.
The actual exported 3104×3000 PNG was pulled and SHA-256 verified; its full
8-bit RGB pixel data match the earlier APK's output exactly. Record exact
APK hashes and host settings; this evidence is one fixture/runtime and does not
establish physical-device behavior or performance.

An independent, checksum-verified Debian QEMU 10.0.13 user-mode run also passes
with the exact original dynamic executable and original API 28 Bionic
linker/libc/libdl files. This is useful architecture evidence, but it is not an
Android guest or darktable processing test. Individual `powf` ABI, DUP and
simple zero-vector store probes also pass, while prefix snapshots change the
execution context. The evidence does not identify a single defective opcode or
prove a particular translation bug. Keep the diagnostic and host-mode details
with any execution report; these observations do not justify changing desktop
algorithms or disabling production vectorization.
