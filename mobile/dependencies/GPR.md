<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
# GoPro GPR RAW decoding

GPR is a DNG container whose Bayer sensor image is compressed with VC-5.
Darktable's pinned RawSpeed source already implements this compression in
`DngDecoder`, `AbstractDngDecompressor` and `VC5Decompressor`. The normal engine
therefore reads the original GPR directly using its DNG tags and camera
metadata, then runs the same processing/export pipeline as other RAW inputs.
The GoPro SDK is not required for this production decoding path.

The extension belongs in darktable's RAW classification list. Merely accepting
that extension or displaying an embedded JPEG would not establish RAW support;
the decoder and engine tests exercise the actual sensor data. Like other DNG
cameras, GoPro images can use embedded calibration metadata even when a model
has no separate entry in RawSpeed's `cameras.xml`.

The current Android dependency-only build still excludes the desktop engine.
Shipping RawSpeed in that APK does not make it a working RAW/GPR editor. The
experimental complete configured runtime has now been packaged and installed
separately, and one original Fusion GPR opened on an ARM64/API 26 emulator.
Its first native preview crashes; Android GPR editing/export has not passed.
See [port status](PORTING_STATUS.md) for current evidence and limits.

## Optional SDK reference and fallback utility

The optional `DT_MOBILE_WITH_GPR_SDK` CMake option builds the independent SDK helper
and decoder reference tests through
`mobile/cmake/GprSdk.cmake`. It downloads the immutable upstream commit
`446c736a38fb14f51343605c0780d347dc602f89` from
<https://github.com/gopro/gpr> and verifies archive SHA-256
`ddb09f4910f07f43e7d93dbda41d20a6af572be5ac7032052f2fa24b33aeeb0c`.
Offline builds may set `FETCHCONTENT_SOURCE_DIR_DT_GPR_SOURCE` to an extracted
copy of that exact archive. The SDK is compiled with VC-5 reading enabled;
its encoder, RGB preview conversion entry points and sample applications are
not used by the bridge. All build settings stay scoped to the SDK targets.

The SDK's bundled Adobe XMP and Expat share symbol names with the metadata
libraries used by Exiv2. For this reason the bridge is an isolated shared
library, `libdt_mobile_gpr.so`, with hidden SDK symbols and only two exported
C functions. ELF builds also bind internal references locally. It must be
packaged alongside `libdt_mobile.so` and the matching NDK C++ runtime when
explicitly shipping this optional helper in an Android build. Android packaging
is not yet verified. The normal RawSpeed path needs no such extra library.

For this utility, `dt_mobile_gpr_prepare()` first reads source metadata into the SDK's
parameters, then calls `gpr_convert_gpr_to_dng()`. This preserves the metadata
understood by that SDK, including the sensor dimensions, CFA layout, camera
profile, white balance, black/white levels, orientation and lens gain maps.
It does not claim lossless preservation of arbitrary vendor metadata. Keep
the original GPR for archival use.

The bridge serializes SDK access because the upstream parser has shared
scratch buffers. Android callers of the utility supply the app's private cache directory;
other callers can supply a directory or use `TMPDIR` (otherwise `/tmp`).
Temporary files are created exclusively with mode `0600`; failed writes are
removed. The SDK cannot be interrupted mid-conversion, so cancellation takes
effect after the conversion returns. Device memory limits still apply.

## Verification

With `DT_MOBILE_BUILD_TESTS=ON` and `DT_MOBILE_WITH_GPR_SDK=ON`, reference tests use
the six GPR samples shipped in the same pinned official archive: HERO5, HERO6,
HERO7, HERO9 and both Fusion cameras. For each sample the test checks:

- GPR and decoded DNG have the same full sensor dimensions and CFA format.
- Every decoded Bayer sample matches byte for byte before demosaicing.
- The source file remains byte for byte unchanged.
- The cache file is private and is deleted by cleanup.
- Missing and corrupt input return errors without producing a cache path.

`dt_mobile_rawspeed_gpr_test.cpp` independently compares RawSpeed's direct GPR
decompression with the official SDK's sensor samples. Bad-pixel repair,
stage-one opcodes and cropping are disabled for this comparison so it checks
decompression before optional image-processing changes.

The desktop-backed raw engine test starts at `dt_mobile_open()` with the
original GPR, changes exposure, checks that the preview changes, exports a
full-size PNG and decodes that PNG. These tests do not replace Android device
validation of the complete pixelpipe, color management, editor UI and export,
or establish support for every GoPro model or full desktop feature parity.

## Source and notices

GoPro code offers MIT or Apache-2.0 licensing. The pinned source also bundles
Adobe DNG/XMP SDK, Expat and MD5 components under their own notices, collected
in its `LICENSE.txt` and component source headers. Distribution of builds
that include this optional SDK helper must include the pinned SDK's `LICENSE.txt`, `LICENSE-MIT`
and `LICENSE-APACHE`, and the corresponding pinned SDK source with the
application's complete source bundle. A git-only archive of this repository
does not include FetchContent downloads.
