<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
# Android native engine execution test

## Expanded editor controls: current source validation

The current editor exposes the reviewed scalar and fixed-array parameters,
curves, blending and parametric masks, native path/brush forms and node editors,
retouch/spot forms, liquify point warps, native-input-aware crop aspect drafts,
photo-selected tone equalizer adjustments, mask placement, camera white-balance
presets, coupled AgX/Filmic controls, text fields and file imports described in
[control coverage](../mobile/CONTROL_PARITY.md). Runtime metadata depends on
the image, module instances and native capabilities; source-policy field counts
are not counts of independently rendered Android controls.

The current validation includes:

- The coordinated Android check passed production and instrumentation Kotlin
  compilation, Debug and Release each passed 159 JVM tests in 34 classes
  (zero failures, errors or skips), plus `lintDevDebug` and `lintDevRelease`.
  Additional instrumentation-source checks remain separate from
  actually running those component tests on Android.
- The final expanded source passed all seven host native CTest cases in
  238.13 seconds, including the original-engine regression in 237.87 seconds.
  Its RGB fixture traversed 1,197
  advertised ordinary parameters, including live-instance and blending fields;
  this runtime count is distinct from the source-policy field counts. The run
  includes minimum-node and invalid-topology checks for paths and brushes.
  Inactive lens-method controls and automatic-mode manual gamma/hardness are
  hidden; dedicated controls and camera catalogs appear when applicable.
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

Tool-search navigation adds six compiled `WorkspaceToolSearchTest` component
cases. They cover explicit cross-category result selection, complete controls
after selection, query/filter changes without automatic selection, exact
disabled-instance identities, no-match cancellation and busy-state rejection.
These bitmap/callback tests have not been executed on Android in this validation.

Five `WorkspaceTabNavigationTest` cases execute the production Compose workspace
on a host Robolectric 4.14.1 / Android SDK 35 runtime with native graphics and
compiled app resources. They cover delayed previous-tab selection, closing
search on tab navigation, exact disabled-instance restoration, deferring active
selection while an edit is running, and returning from Settings. This is host
UI execution using bitmap/callback fixtures, not physical-device or native
image-engine validation. Both Debug and Release passed all five cases without adding
a test Activity to the application manifest. Gradle prepares the pinned host SDK
under the build directory; Robolectric uses it offline instead of maintaining
a second downloader and lock in the user's home directory.
Re-running the two targeted cases against the preceding `5acf686182` workspace
reproduced both failures: search stayed over the destination controls, and a late
crop selection stayed active after returning to Light / Color. The fixed
workspace passed all five host navigation cases.

```sh
android/gradlew -p android :app:testDevDebugUnitTest \
  --tests org.example.darktableandroid.WorkspaceTabNavigationTest
android/gradlew -p android :app:testDevReleaseUnitTest \
  --tests org.example.darktableandroid.WorkspaceTabNavigationTest
```

Six `WorkspaceTabPairsTest` cases passed in Debug and Release. They use the
production `ModuleControls`, with physical
pointer taps across all six directed pairs of the three editing tabs. It covers
Exposure and mandatory Orientation (`flip`) before and after Detail / Repair,
including EV and rotation callbacks, non-default instances, white balance and
lens controls, and saved crop drafts/scroll with an offscreen native refresh.
The click-event assertion requires the remembered target to become active before
recomposition. On the preceding `6bab601832` workspace, that assertion failed
with `flip` still active after the Light / Color click. The other eventual-render
checks passed in the host runtime; this baseline does not reproduce the device's
persistent stale-render symptom. The new explicit tool composition key also
prevents cross-tool node reuse; physical-device rendering remains unverified.

Eight `IncomingPhotoIntentTest` and four `ManifestShareIntentTest` host cases
passed in Debug and Release and cover single-file Android sharing. The actual merged MainActivity manifest is
resolved for GPR/image MIME types, opaque generic-binary document URIs and a
wildcard sender, with no synthetic share target registered. Existing RAW
View/Edit matching remains covered. Parser checks exercise stream precedence,
ClipData/data fallbacks, unchanged grant flags, malformed extras and rejection
of missing, multi-file or unsupported-URI payloads. These do not launch the native
engine, transfer real provider grants, execute an OEM file manager/share sheet,
or establish a new GPR decode result.

Three `DarktableThemeTest` host cases passed in Debug and Release. They render the production workspace and module
controls under the previous Material tokens and the desktop-grey theme. They
compare measured bounds, touch/semantic actions and typography, and exercise
search, exact disabled-tool selection, Settings restoration, toolbar actions,
numeric Apply, boolean and power callbacks. Native-graphics view captures also
check the neutral `#777777` photo surround and produce PNG previews under
`android/app/build/reports/theme-preview/`. The example JPEG/bitmap is a UI
fixture, not native RAW rendering evidence. The Material-token comparison uses
the same current workspace, not a screenshot of a previous APK. Neither these
host captures nor the platform colour changes establish physical-device
system-bar or document-picker execution.

The lens regression uses the installed Canon EOS 5D Mark II / EF 24–105mm f/4L
IS USM calibration with the original RGB pixelpipe. Distortion, vignetting and
TCA each change actual pixels, Undo restores the baseline, and manual vignette
has exact Undo/Redo checks. A deliberately unmatched body with a valid lens
guards the processing crop-factor fallback; invalid profile selections and
numeric settings leave metadata and history unchanged. Separate production
classifier fixtures distinguish identity, CA-only, geometric, vignette,
tangential-only and invalid DNG coefficient data. These are not HERO12 RAW
rendering evidence. Profile-action aliases and native buffer limits are
covered by JVM tests. See
[lens correction and HERO12 scope](LENS_CORRECTION.md).

The updated official Lensfun database was converted with the pinned upstream
converter and lxml 6.0.2, then loaded by the original 0.3.4 library: 56 XML
files, 1,059 cameras and 1,578 lenses. The final native run explicitly required
that database with `DT_MOBILE_TEST_REQUIRE_UPDATED_LENSFUN=1`; canonical mobile
profile actions found both HERO10 and HERO11 pairs and their distortion
capabilities. Desktop adapter runs using older distribution data report those
optional pairs as absent. The automatic-default fixture separately verifies
that unknown HERO12 EXIF cannot select HERO4, read-only metadata leaves stored
parameters unchanged, and reset/method changes cannot revive that automatic
selection. Explicit legacy settings remain unverified and preserve their
existing behavior. All 37 offline database/staging tests passed. The six lens
component tests compile, including the screenshot regression; they have not
been executed on Android. The preceding `8f1cf85e4b` snapshot passed both the
complete engine-APK workflow and Android CI with the updated dependency overlay.

Tone equalizer's [photo-selected adjustment](TONE_EQUALIZER_TOUCH.md) adds five
JVM geometry/target model tests, eight target-queue tests and ten compiled
Compose component tests. The original-engine
regression samples the actual filtered mask without changing metadata/history,
rejects invalid/off selections, changes multiple exposure bands in one history
step, checks actual pixels and exact Undo/Redo, and isolates additional module
instances. Changing its mask exposure changes the input sample, while changing
only its nine gains leaves that input stable and changes the selected output.
An oriented image with a crop after
tone equalizer checks the mapped point against its uncropped reference at the
same mask resolution; the deliberately wrong direct coordinate differs.

The preceding live-tone production JNI bridge compiled with NDK 27.2 for AArch64/API 26 and
linked against the host engine with no undefined symbols. A real host JVM
`-Xint` probe changed all nine gains through the grouped action, added exactly
one history step, verified changed pixels and exact metadata/pixel Undo/Redo,
and rejected 24 invalid/no-preview/disabled/wrong-module/target calls without state
changes. The RGB source hash remained unchanged. This verifies JNI and native
behavior, not execution of the new Android touch component on a device.

The live-adjustment regression additionally reproduces input samples at
`+4`, `+8` and `-12 EV`, outside the processor's `[-8,0] EV` interpolation
domain. Adjustments now target the same boundary bands that processing uses.
Selected output queries evaluate the actual quantized LUT, rather than adding
the requested offset to the unchanged input sample. A real mask boosted by
8 EV changed both the reported output and actual preview pixels. The JNI run's
8.269397 EV input changed from output 8.268071 to 8.368225 EV after a 0.1 EV
target increase; separate +8/-12 targets were reached within 0.002 EV.
Consecutive live targets share one gesture history snapshot, while a new
gesture, another edit or Undo ends the grouping. Exact target Undo/Redo,
unreachable limits and invalid inputs are covered. The JVM queue tests cover
latest-target coalescing during in-flight work, duplicate finish events and
stale selection/completion rejection. Compose sources cover immediate fine
buttons, streamed slider targets, preserved gestures across preview updates and
Selected tone feedback/rejection. These component sources compile; no Android
device or emulator was available for their execution.

The [additional controls](ADVANCED_CONTROLS.md) add six WB preset metadata JVM
tests, five mask projection JVM tests, and fourteen compiled Compose component
tests. Original temperature-plugin fixtures verify real catalog grouping and
interpolation, four-channel exact entries and rejected interpolation gaps,
inactive-channel preservation and calibrated gains above the manual limit.
LDR/monochrome/unmatched fixtures omit unrelated catalogs. These are catalog
and parameter fixtures, not a new camera-specific RAW rendering result.

AgX/Filmic tests compare original primaries, range/pivot, grey-point and
automatic hardness/gamma behavior, including atomic invalid-value rejection
and automatic-mode visibility. Real RGB renders check native action and range
changes, exact history and pixel Undo/Redo. Photo-mask tests cover atomic centre
and clone-source placement, crop/orientation, fractional pixel aspect ratio,
perspective and off-image rejection, inactive lens geometry, exact history and
pixel restoration, and omitted rotated path/brush source handles. The pixel
aspect callbacks now use the same committed input/output ratios as processing,
instead of the previous swapped-axis resampling-ROI calculation.

The three new JNI methods compiled for AArch64/API 26 and linked against the
host engine with no undefined symbols. A real host JVM probe verified LDR
preset rejection, read-only mask projections, changed pixels after placement,
one history item for both axes, and exact metadata/projection/pixel Undo/Redo.
All 25 invalid requests rejected without mutation and the PNG source hash
remained unchanged. These compiled component tests have not run on Android.

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
