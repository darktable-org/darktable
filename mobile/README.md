<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
# Mobile engine integration

The target is the existing Android single-image interface with the desktop
darktable processing algorithms, RAW inputs (including GoPro GPR), and desktop
export capabilities. A lighttable, gallery scan, and image browser are outside
this interface. Darktable's internal image/history database is still required
by its processing core; it does not imply a browser screen.

The experimental Android ARM64/API 26 core, JNI and configured modules now
build and link with real GTK3 Broadway, and their installed runtime has been
staged and verified. An experimental engine-enabled APK has passed final
runtime byte/hash verification. The default Gradle engine remains disabled.
A preserved APK pair passed the complete JNI Fusion GPR test on ARM64/API 28:
real previews, exposure, native Kelvin/tint, exact undo/redo, full-size PNG export
and Android decoding, source protection and close/reopen. That run required
host single-step emulation with fixed virtual timing; it is one fixture/runtime,
not full desktop parity or device performance evidence. The newer default-instance
APK pair also passed its complete 41-phase JNI run, including instance creation,
independent edits, topology undo/redo and retired-selector rejection.
See [port status](dependencies/PORTING_STATUS.md) for the dated evidence and
remaining limits.

## Implemented boundary

`dt_mobile.h` exposes original module metadata, numeric and bounded text/file
updates, dedicated curves, atomic white-balance/crop batches, module defaults,
instances, session history, preview and native-resolution export. The adapter
uses upstream introspection, develop history/masks and the imageio pixelpipe.
Original module hooks maintain retouch/clone forms, standalone liquify warps and
zone partitions; the bridge does not implement alternate processing formulas.

The reviewed policies contain 511 scalar fields and up to 440 fixed-array
components across 81 modules. The component count includes only reviewed channel/
band addresses and depends on active patches and image applicability. Dedicated
curves, masks, blending, synthetic zone/per-form controls, file/text parameters and
action buttons are additional capabilities, not entries in that count. See
[control coverage](CONTROL_PARITY.md) and the policy headers for the identities,
source reviews, hard/soft bounds and exact remaining desktop interactions.

JSON descriptions contain `operation`, `name`, `enabled`, `canDisable`,
`canCreateInstance`, `canReset`, `parameters`, `textParameters`, `curves` and
optional crop/RGB-curve/white-balance descriptions. Numeric parameters contain
native type/value/bounds, optional image-specific defaults, optional soft slider
ranges and native/synthetic choice labels. Parameters with `type: "action"`
become buttons, not numeric editors. Text fields specify UTF-8 `maxBytes`; file
imports additionally advertise `file: true` and permitted `extensions`.

Metadata and setters share type/field policies and image-dependent checks.
Sensor black/white preparation and demosaicing controls are omitted for
inapplicable inputs. Enum sentinel/automatic-picker entries are filtered; Bayer,
X-Trans, four-Bayer and monochrome method menus follow the original source.
Native validation maintains ordered levels/bands/curves, exposure and filmic
luminance relations, AgX targets/gamut, normalized mixer rows, retouch scale
relationships and sensor white/black separation. Readable imported values do
not become writable merely because they appear in metadata. Unknown structural
fields and opaque arrays remain inaccessible to ordinary numeric/text updates.

Android presents sliders with precise input, choice menus, switches, native reset
and action buttons, tool/parameter search and the dedicated editors. A numeric
drag submits one history/preview operation on release. Crop and white balance
retain dependent values in a draft until one atomic Apply. Internal history/
diagnostic IOPs are hidden without changing their native identities. The legacy
`colortransfer` IOP has no current desktop editing widgets either; Android keeps
its original explanation/enable state instead of exposing private fields.

The bridge now commits uniform blending, available preceding raster-mask
sources, original circle/ellipse/gradient forms and per-form repair settings via
their native state operations. External raster imports set the original path and
basename together; LUT imports set the path and clear obsolete compressed state.
The original readers accept the retained session documents. Overlay image
imports use the original image importer, synchronized dependency state and its
image-ID/filename tuple, with self/cycle rejection. These files must remain
available while undo/redo can restore references to them. Brush/path and
parametric-mask editors, linked liquify strokes, automatic sampling and full
preset workflows still need further integration.

`dt_mobile_set_curve` updates complete node lists and native interpolation for
basecurve, RGB/Lab tone curves and color zones. Metadata provides native samples,
channel limits and image-specific reset nodes/interpolation. The RGB options
batch preserves the original linked/independent-channel transition rule. Fixed
channel vectors, levels and frequency bands use reviewed component controls;
zone counts/automatic positions use their original coupled partition operation.
Resetting module settings preserves enabled state, blending and masks, rebuilding
retouch/clone bookkeeping through the original module hooks.

Previous ARM64/API 28 Compose/JNI evidence and current host/JVM/build checks are
reported in [native verification](../android/NATIVE_ENGINE_TESTS.md). They are
separate scopes; the default Android dependency-only build still disables the
processing engine. A matching engine, original plugins and complete runtime data
must be packaged before the interface can process an image.

The separately recorded first-PNG-preview observation remains relevant to pixel
checks: regressions explicitly prime the unchanged original pipeline before
requiring stable baselines and exact undo/redo. See the verification record for
current results rather than inferring processing parity from widget counts.

`dt_mobile_set_whitebalance_gains` applies all active coefficients together,
selects the original user-defined setting and clears its late color-adaptation
mode in one history step. The reviewed manual range is `0.001f..8.0f`; this is
narrower than the desktop slider's zero-inclusive range. Inputs are never
silently clamped or green-normalized. `dt_mobile_set_whitebalance_setting`
selects the image's original as-shot (0), camera-reference (3), or as-shot-to-
reference (4) coefficients and processing mode together. Available settings
come from the current develop's original coefficient sources; valid named
coefficients above 8 remain intact and visible.

White-balance metadata supplies active channel indexes/labels/current values,
manual bounds, current setting and available named settings. Three-channel
edits preserve the inactive fourth coefficient, including its NaN sentinel.
Four-Bayer channel names and CYGM display order follow the desktop; the JNI
array remains in native index order. True camera-monochrome inputs omit/reject
the editor. Non-RAW inputs retain the desktop's optional manual processing.
Snapshots retain chroma state, and restore synchronizes it with the actual
temperature module before subsequent operations.

`dt_mobile_set_whitebalance_temperature` applies Kelvin/tint together using the
desktop temperature module's original spectral integration, camera matrices and
tint convention through `common/whitebalance`. The desktop GUI uses that same
extracted core; Kotlin supplies no second formula. The original Y-scaling tint
convention and blackbody/daylight transition remain unchanged.
The native description reports current values and original bounds only when
calibration/current gains are usable. An Apply normalizes native channel 1 (green for RGB) to one, preserves
the inactive fourth channel, selects user-defined balance and records one undo
step. Positive native gains above the manual maximum remain intact; a pair that
produces invalid camera gains fails without changing state. Spot sampling and
camera-database preset selection still need dedicated support.

`dt_mobile_set_crop` applies a complete normalized left/top/right/bottom
rectangle through the desktop crop module in one history change. It validates
the upstream field layout and a minimum extent of `0.01f`, comparing minimum
edges using the desktop widget's float addition. The batch also selects the
desktop freehand aspect setting, so an earlier fixed ratio cannot change the
requested rectangle during export. Crop metadata contains the four edges,
`minSize` and whether the current parameters already use freehand proportions.
These coordinates belong to the crop module's input space. The Android editor
uses numeric coordinates and a separate rectangle diagram; it does not treat
the already cropped preview as the original image. Fixed aspect presets and a
touch overlay on an uncropped image remain to be implemented.

`dt_mobile_history`, `dt_mobile_undo` and `dt_mobile_redo` expose the current
session's edit window. Each successful Apply creates a separate snapshot, even
when desktop history merges consecutive changes to one module. The window holds
up to 64 steps; editing after undo discards redo. Snapshots use the original
history, module-order and mask-copy routines. Undo restores the original
parameters and database history used by the preview/export pixelpipes. These
steps do not persist after closing the image and do not expose automatic startup
history as user edits. The Android toolbar disables unavailable actions and
serializes history changes with editing and rendering.

`dt_mobile_new_instance` creates an enabled instance using the original desktop
creation/order and image-specific default-parameter helpers. It returns a
caller-owned `operation@priority` identifier. Only modules advertising
`canCreateInstance` support it; single-instance and hidden modules reject the
operation. The new instance uses default blending and does not copy the source
instance's edits or masks. Undo removes it from the active topology, redo restores
its identifier and settings, and discarded redo branches do not reuse identifiers.
Snapshots retain retired instances only while reachable; abandoned instances are
reclaimed. The Android editor focuses the returned instance and routes its controls
through the same native APIs. Copying current settings/masks and deleting or
reordering instances still need separate support. Native host regressions cover
pixel-exact restoration and actual cleanup. The exposure-instance sequence also
passed on the ARM64/API 28 Fusion fixture with the recorded host settings; other
modules and device/runtime combinations still need validation.

`dt_mobile_set_module_params` is a low-level interface for trusted native
integrations that already own a validated upstream parameter struct for the exact
engine version. It checks the byte size, not cross-field validity, and must not
receive arbitrary input or UI-provided blobs. Android does not expose it through JNI.

The Android editor reads these descriptions and calls the JNI adapter. Export
encodes to a private cache file at native resolution before copying the completed
file to a document selected by the user. Preview bitmaps are not export inputs.
Native exports use a temporary destination and rename on success; the public API
rejects destinations that refer to the original input. Session operations are
serialized by the owner; only cancellation can overlap them, and close waits
for the owning job to finish.

`dt_mobile_export_with_settings` adds aspect-preserving maximum dimensions
(zero means unrestricted, without upscaling), PNG 8/16-bit unsigned output,
TIFF 8/16-bit unsigned or 32-bit float output, format-specific compression,
JPEG quality/subsampling, JPEG/TIFF DPI, available built-in output profiles,
rendering intent and a source-metadata policy. Format metadata advertises the
supported controls. The original format plugins and color-management pixelpipe
perform the encoding. Temporary format configuration remains active through the
writer and is restored on success, failure and cancellation. The legacy export
API retains its quality range, including the desktop JPEG minimum-quality clamp.
“Do not copy source metadata” suppresses source EXIF/XMP copying; required encoder
fields and ICC profiles can remain. Custom ICC files, half-float output, layers
and the remaining codec-specific options still require dedicated support.

`dt_mobile_configure_runtime(data_dir, module_dir, temp_root)` copies absolute
paths before the first image is opened. It rejects incomplete startup data,
unwritable temporary storage, and changes while a session or runtime init/cleanup
is active. The configuration survives closing the last session. Config, cache
and pixelpipe temporary files stay inside a fresh private child of `temp_root`;
the child is removed when the runtime closes. Host tools may instead keep using
`DT_MOBILE_DATADIR` and `DT_MOBILE_MODULEDIR` without configuration. The API never
changes process environment variables. Android prepares its versioned data tree
and configures `applicationInfo.nativeLibraryDir` through this entry point.

`dt_mobile_capabilities()` reports the processing engine only when it is linked.
A dependency-only build rejects open with `DT_MOBILE_ERROR_UNSUPPORTED`, rather
than returning a successful session that can never process an image.

## GoPro GPR

The pinned desktop RawSpeed includes a VC-5 decoder in its DNG processing path.
The mobile desktop-engine build reads the original GPR directly through this
same decoder, and GPR is classified as RAW rather than LDR. GPR support does not
require a second production decoder or an embedded JPEG preview.

`DT_MOBILE_WITH_GPR_SDK=ON` additionally builds the official GoPro SDK as an optional
independent reference decoder and supplies its six real verification samples.
It uses immutable commit `446c736a38fb14f51343605c0780d347dc602f89`, with the archive
SHA-256 verified in `cmake/GprSdk.cmake`. Its isolated helper can decode GPR into
a private DNG cache, but the desktop-backed mobile build continues to use
RawSpeed directly. The helper never changes the original source, and removes
temporary files on failure or cleanup.

The GPR capability requires a linked processing engine with its VC-5 decoder.
Building only the reference SDK does not make the Android application able to
edit RAW files. The optional SDK contains Adobe DNG/XMP and other third-party
code: its corresponding licenses and source must accompany any distributed
build that includes it.

## Verification

For an opt-in processing-library build from the repository root, set
`BUILD_MOBILE_LIBRARY_ONLY=ON`. This also enables the mobile engine and keeps
`lib_darktable`, all configured processing and format/storage modules, and runtime data,
while excluding desktop views, lighttable plugins, executables, documentation
and packaging. Build `dt_mobile_runtime` to produce the complete dynamic module
set. This mode is independent of Gradle's current engine-disabled dependency
probe and does not establish Android build, packaging or device support.

Dependency-free public API and adapter-boundary tests:

```sh
cmake -S mobile -B build/mobile -DDT_MOBILE_BUILD_TESTS=ON \
  -DDT_MOBILE_WITH_DARKTABLE_ENGINE=OFF
cmake --build build/mobile
ctest --test-dir build/mobile --output-on-failure
```

These tests cover unavailable-engine reporting, argument validation, ownership,
error propagation, cancellation and source-file protection. The mock adapter
test does not establish RAW decode support.

To test the actual VC-5 decoder independently, add `-DDT_MOBILE_WITH_GPR_SDK=ON` to
the same configure command. The SDK archive includes six real official samples:
HERO5, HERO6, HERO7, HERO9 and both Fusion lenses. Each test verifies full image
dimensions, every decoded Bayer sensor byte through a GPR-to-DNG round trip,
unchanged source data and private cache cleanup.

With the desktop engine also enabled, `dt_mobile_raw_engine_test` opens each
original GPR through the public API and RawSpeed, changes exposure, exports a
full-size PNG, decodes that PNG, and confirms the source GPR remains unchanged.
`dt_mobile_raw_whitebalance_GPBK7066` additionally uses the original Fusion GPR
to exercise native white-balance settings/gains and Kelvin/tint, pixel-exact undo/redo and an
edited full-size PNG round trip. Its CLI equivalent adds `--whitebalance` to
the existing raw engine test; the other six sample tests keep their original
behavior. These remain host tests, not Android execution proof.

The real desktop-backed adapter must be configured from the repository root
with `BUILD_MOBILE_ENGINE=ON`, `DT_MOBILE_BUILD_TESTS=ON` and the normal desktop
build dependencies/submodules. Build target `dt_mobile_engine_test`, then run
`ctest --test-dir <build-directory> -R '^dt_mobile_engine_test$'
--output-on-failure`. The test changes exposure and curve nodes using upstream
processing, checks that rendered pixels change, rejects invalid curve batches
without changing module state, and verifies exported dimensions exceed the
preview and match the input. It also checks pixel-exact session undo/redo,
crop undo dimensions, the 64-step window, real PNG/TIFF precision and
compression tags, non-square bounded exports without upscaling, embedded ICC
selection, JPEG subsampling/DPI, original-source protection and exact temporary
configuration restoration, including command-line override conflicts.
White-balance checks cover gain- and Kelvin/tint-dependent pixels, exact undo,
named setting semantics and inactive-fourth-channel preservation. With LCMS2
available, the standalone `dt_mobile_temperature_test` also compares the shared
spectral core to golden results from the original desktop source, including both
sides of its 4000 K transition. These tests do not establish Android execution.
Cancellation after the real PNG encoder has written its temporary file also
checks that the destination stays absent, the temporary file is removed and
the original raw configuration is restored.
The `desktop-engine-adapter` job in
`.github/workflows/android-ci.yml` records the complete dependency/configure
commands. This is a desktop host test, not Android device validation.

## Work still required for the complete target

* Validate Android native loading, module discovery and runtime initialization
  using the built/staged and APK-verified core, JNI and configured
  processing/format/storage modules. Enabling the default Gradle engine still
  requires device proof.
* Complete runtime data and initialization for remaining desktop capabilities,
  including Android font discovery/Fontconfig, text watermarks, optional codecs
  and integrations. The verified stage retains the configured darktable data
  and Lensfun database; the default three flat Android database assets alone
  remain insufficient.
* Complete advanced brush/path and parametric-mask controls, linked liquify
  strokes, white-balance spot sampling and camera preset/tuning, automatic
  sampling/range helpers, fixed crop proportions, an uncropped touch overlay
  and automatic perspective fitting. Add persistent editing history, styles/
  presets and instance copy/delete/reorder operations. Existing scalar/vector,
  curve, mask, repair and blending editors do not establish every desktop
  widget or canvas gesture.
* Complete the export settings beyond the current dimensions, PNG/TIFF precision,
  compression, JPEG subsampling, DPI, built-in ICC profiles/rendering intent and
  source-metadata policy: custom ICC files, layers and other codec-specific
  options remain pending.
* Establish a camera/format sample matrix matching the desktop build, including
  optional RAW decoder fallbacks, and compare identical histories/exports.
  Validate Android ARM64 builds and actual devices for memory, interruption,
  lifecycle and complete RAW-to-edit-to-export behavior.

Until these are finished, the repository must not describe its APK as supporting
all desktop RAW types, all desktop tools, or standalone GPR editing.
