<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
# Android build

Open this `android/` directory in Android Studio. Install Android SDK 35, NDK 27.2.12479018, and CMake 3.22.1. Build `devDebug` or run `./gradlew :app:assembleDevDebug`; install with `adb install -r app/build/outputs/apk/dev/debug/app-dev-debug.apk`.

The Android UI opens one document directly into a fixed split editor: preview
above, tool controls below, with four persistent bottom tabs and no app-title
bar or dismissible tools sheet. The three editing tabs group light/color,
detail/repair and geometry/effects; Settings contains the license information.
Selecting a tool does not enable it: its separate power switch controls whether
its edits apply. Each category remembers its tool, and each tool keeps its scroll
position. See [editor UX](EDITOR_UX.md) for the interaction rules.

An engine-enabled build also opens a single photo shared from Android's file
manager. Share a GPR file and choose this editor; image MIME types and generic
`application/octet-stream` shares are registered, including GoPro application
MIME aliases. The incoming document can use `EXTRA_STREAM`, a single-item
`ClipData`, or the sender's data URI. Original read-grant flags reach the existing
content-URI cache/import flow, which copies the source privately before native
processing. The share URI does not need a visible `.gpr` suffix. Multi-file shares
are not advertised by this single-image editor.

The tools panel provides native sliders and precise input, enum
menus, switches, text fields, document import and action buttons. Tool search
matches module names and parameter labels across all editing tabs. Tap a search
result to open that tool and its category without changing its power state.
Editing a value commits the original
module/history and requests an original-engine preview. A completed slider drag
creates one edit. Image-specific parameter resets and tool settings resets use
native defaults. Progress and cancellation remain visible beside the preview.

The source policies cover 511 reviewed scalar fields and up to 440 fixed-array
components across 81 modules. Actual controls depend on sensor type, active
patches, selected mode and installed plugins. These counts exclude curve nodes,
zone controls, blending, native mask/retouch/liquify operations and text/file
fields; action buttons are not numeric parameters. See
[control coverage](../mobile/CONTROL_PARITY.md) for exact scope and remaining
advanced desktop interactions.

Dedicated editors include base/RGB/Lab tone/color-zone curves, atomic crop and
white balance, levels and fixed frequency bands, and the original zone system.
Crop aspect presets and portrait/free drafts use the native input ROI for each
crop instance, including preceding geometry changes, and apply complete bounds
atomically. Parametric masking groups original input/output channels with four
ordered threshold sliders, inversion, boost/reset and native mask refinement.
The native bridge also supports blending and preceding raster-mask selection,
native circle/ellipse/gradient/path/brush forms, clone/retouch shape controls and
standalone liquify warps. Stable form and node selectors expose precise path
coordinates, Bézier handles and brush feather/hardness/density controls, up to
64 nodes per form. Subsequent group members support the original five
combination operations. Spots supports clone paths; retouch supports paths and
brushes. Applied coordinate diagrams remain separate from the processed photo.
Lens correction has a dedicated mode-aware editor with camera/lens profile
selection, shooting settings, actual source/profile capability reporting and
manual vignette controls. It resolves original image defaults before edits and
retains a valid crop factor when a camera body has no database match. HERO12
Black has no bundled profile; embedded TCA/shading data does not establish
fisheye correction. See [lens correction](LENS_CORRECTION.md) for usage and exact
limits. Watermark text/font/template uses bounded native text fields.
System document selection imports external raster
PFM/PNG masks, Cube/3DL/Hald-PNG LUTs and PNG/JPEG/TIFF overlay images into retained
session assets. The original readers and processing code handle those files;
overlay selection also updates its original image ID and dependency state. The
UI supplies no replacement image formulas.

The tools panel hides internal pipeline/diagnostic modules. RAW-only settings
and methods follow the input's original capabilities, including Bayer/X-Trans/
four-Bayer/monochrome demosaicing. The removed desktop `colortransfer` editor
retains its legacy explanation/enable state. Freehand stroke capture, direct
photo-canvas manipulation, live channel histograms, linked liquify strokes,
automatic pickers, full preset browsers and other advanced interactions remain
outside this coverage.

The default Gradle build still sets `DT_MOBILE_WITH_DARKTABLE_ENGINE=OFF`.
That dependency-only route reports processing unavailable and disables image
opening. The explicit [prebuilt route](PREBUILT_ENGINE.md) packages an installed,
verified engine/runtime stage. Earlier ARM64/API 28 preview/edit/export runs and
current checks are recorded separately in [native verification](NATIVE_ENGINE_TESTS.md)
and [port status](../mobile/dependencies/PORTING_STATUS.md). Their exact fixtures,
execution settings and limitations matter; compilation does not establish full
desktop parity, physical-device behavior or performance.

Module instances use image-specific original defaults and keep independent
settings. Session undo/redo retains up to 64 successful edit steps; editing after
undo discards redo. Imported files remain available while their session history
can reference them. Copying current settings/masks and deleting or reordering
instances remain pending.

The export destination uses Android's system document picker. Settings come from
the installed native formats, including dimensions without upscaling, supported
precision/compression, JPEG quality/subsampling, DPI, built-in output profiles,
rendering intent and source-metadata copying. Export renders at native image
resolution; the preview bitmap is not its input. Further optional format and
custom ICC-file parity is documented separately.

## Native runtime data

`RuntimeAssets.initializeNative` prepares and configures an explicit engine-enabled
build. It requires the complete matching runtime tree under APK assets
`darktable-runtime/`, recursively extracts it into a private, installation
version-specific directory beneath `filesDir/darktable-runtime/`, and commits
the tree only after a successful copy. It never merges a new installation over
an old tree. A failed extraction is not reused; previously committed versions
are retained so an active native session cannot lose its files. The native data
directory is the committed tree's `data/` child; sibling resources such as
`lensfun/version_1/` retain their layout relative to that data directory.

The minimum checks require nonempty `darktablerc`, `rawspeed/cameras.xml`,
`noiseprofiles.json` and `wb_presets.json`. The first is generated by the desktop
build, and the others are read directly by the RAW decoder, denoise profiles and
white-balance initialization. These checks are necessary startup conditions;
they do not establish a complete runtime. Package the matching installation's
data, including styles and watermarks, any supplied `color/in` and `color/out`
profiles, and the data required by enabled optional features such as OpenCL
kernels or AI models. Lensfun calibration data uses the separate sibling
`lensfun/version_1/` directory, retained and verified by the complete runtime
staging route. The lens editor reports a missing database separately from a
missing camera/lens calibration; neither condition implies complete lens parity.

The existing Gradle asset task only supplies three flat camera/noise/white-balance
database files. It does not produce this runtime tree, so an engine-enabled build
with only those files fails preparation with an explicit missing-runtime error.
`tempRoot` uses the application's cache directory. `moduleDir` is always
`applicationInfo.nativeLibraryDir`: processing and export libraries must be
packaged as Android native libraries and extracted by the installer. The helper
rejects `.so` files and ELF content in data assets; it never copies executable
code to `filesDir` or changes process environment variables. A successful native
configuration is retained for the process. A recreated editor reuses identical
paths while its previous ViewModel finishes closing a session; a changed runtime
still goes through native validation and cannot replace an active session's data.

The experimental [runtime staging tool](../mobile/dependencies/STAGING.md)
prepares this tree and its native-library closure from a complete installed
Android engine. The explicit [prebuilt engine route](PREBUILT_ENGINE.md)
consumes the verified stage when selected by a project property. Native
build/link, staging and final APK runtime-content validation have passed for the
configured bootstrap. A preserved APK pair also passed the separate ARM64/API 28
Fusion JNI edit/export test with the recorded host execution settings. Disabled
optional codecs, unported controls and broader device/format checks remain separate work.

## Restoring the Gradle Wrapper binary

The wrapper scripts and `gradle/wrapper/gradle-wrapper.properties` are intentionally kept in this repository. This change omits the newly-added `gradle-wrapper.jar` because the Codex pull-request transport used for this change cannot represent new binary files. Before building, restore the official wrapper JAR in a separate local commit from a trusted Gradle installation:

```bash
cd android
gradle wrapper --gradle-version 8.14.4 --distribution-type bin
git add -f gradle/wrapper/gradle-wrapper.jar
git commit -m "Add official Gradle wrapper JAR"
```

Review the generated changes before committing; the command should agree with the checked-in Gradle 8.14.4 wrapper properties. Do not download an arbitrary JAR or replace the wrapper with a custom binary. The wrapper JAR is the only expected binary source file; APKs, AABs, native libraries, build directories, and signing material must remain untracked.

CI and release jobs generate this ignored wrapper JAR from the pinned Gradle 8.14.4 installation before wrapper validation and every Gradle build. Therefore fork pull requests and release runners do not depend on a binary being committed to the repository.
