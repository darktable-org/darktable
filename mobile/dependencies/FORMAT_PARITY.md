<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
# Android format coverage and remaining parity

Source/configuration audit of the bootstrap used for the `e084d6f085` APK,
with the subsequent XCF export increment described below.
Compilation and advertised extensions do not establish camera, compression or
export-setting coverage. The current evidence is six official GoPro GPR samples
on the host; Android results are tracked separately in [PORTING_STATUS.md](PORTING_STATUS.md).
Optional capabilities also depend on each desktop binary's build configuration.

Keep the original decoder pins: LibRaw
`f74ddd995c5447458f132a5377ca0f4b394dff6e` and RawSpeed
`7cf3dc3b9d9c82b414198b1f57460478be6c6c9d`.

| Original route | Bootstrap configuration | Remaining work |
| --- | --- | --- |
| RawSpeed camera RAW/DNG | JPEG, zlib and original GoPro VC-5 enabled | Real camera/compression/bit-depth matrix |
| LibRaw CR3/CRX and X3F | Pinned decoder; original default extension allowlist is `cr3 x3f` | CR3 is independent of libheif; X3F tools are separately disabled |
| JPEG, PNG, TIFF input/export | Original libraries and modules compiled | Android format/precision/compression/settings fixtures |
| PFM and binary PNM input; PFM/PPM export | Original paths compiled | Other PNM signatures use the unavailable exotic loader; format-specific settings and fixtures remain |
| RGBE HDR and QOI input | Original input sources compiled | Actual fixtures; this source has no corresponding RGBE/QOI export module |
| PDF export | Original module compiled | Layout/compression/font settings and font runtime validation |
| Copy original | Module compiled | Deliberately excluded from processed-output enumeration/export by the mobile adapter |
| JPEG 2000 input/export | `USE_OPENJPEG=OFF` | Original OpenJPEG integration, pinned port 2.5.3 |
| WebP input/export | `USE_WEBP=OFF` | Original integration, libwebp 1.5.0#1 including webpmux |
| OpenEXR input/export | `USE_OPENEXR=OFF` | OpenEXR 3.3.2, Imath 3.1.12 and libdeflate |
| Rendered JPEG XL input/export | `USE_JXL=OFF` | libjxl 0.11.1#1 and highway; this is separate from JPEG XL inside DNG |
| AVIF input/export | `USE_AVIF=OFF` | libavif 1.3.0 plus a working decoder/encoder; dav1d alone does not encode |
| HEIF/HEIC/HIF input/export | `USE_HEIF=OFF` | libheif 1.19.8#1 and appropriate subcodecs; decode/encode, brands and bit depths differ |
| XCF export | `USE_XCF=ON` in the updated bootstrap | Original pinned libxcf, zlib and libm; 8/16-bit unsigned and 32-bit float controls exposed; Android execution tracked below |
| XCF and other exotic input | GraphicsMagick/ImageMagick disabled | Original exotic loader; enabling XCF export does not enable XCF import |

The updated eight configured export modules are `copy, jpeg, pdf, png, ppm, pfm, tiff, xcf`.
Excluding copy leaves at most seven processed-output choices, subject to original
module initialization. Dedicated mobile settings currently cover JPEG, PNG,
TIFF and XCF. Merely compiling another codec does not expose its desktop quality,
lossless, encoder, precision or other options. Preserve each original parameter
layout and invariants when adding those controls.

## RAW paths that need separate verification

- RawSpeed contains DNG compression branches 1 (uncompressed), 7 (lossless JPEG),
  8 (deflate), 9 (GoPro VC-5) and 34892 (lossy JPEG), with JPEG/zlib enabled.
  These branches are not disabled by the unrelated rendered-file codec switches.
- `ENABLE_X3FTOOLS=OFF` leaves the pinned LibRaw X3F parse/decode helpers as
  stubs. The bundled desktop wrapper also defaults to OFF, so this is not an
  Android-only regression. The original adapter additionally requires single-channel
  `rawdata.raw_image`, while the X3F loader fills non-CFA `color3_image`. Enabling
  the flag alone therefore does not establish Foveon support; this needs proper
  original non-CFA processing integration, not an approximate channel conversion.
- `ENABLE_6BY9RPI=OFF` omits additional BRCM RAW handling. This alone does not
  establish that every Raspberry Pi camera path is unavailable.
- DNG 1.7 JPEG XL compression 52546 is absent from this RawSpeed pin's switch;
  this LibRaw path is an unsupported-format placeholder. The original alternative
  needs DNG SDK 1.7+, `USE_DNGSDK` and `qDNGSupportJXL`, plus the original LibRaw
  extension allowlist. Turning on root `USE_JXL` alone only adds rendered JXL.
- Bundled LibRaw's Jasper/RedCine option is disabled by root desktop CMake too;
  it is distinct from darktable's OpenJPEG import/export switch.

The LibRaw wrapper's `libraw_extensions` configuration also matters: the presence
of LibRaw does not mean every RawSpeed failure automatically falls back to it.

A work-only CAPI probe linked to the unchanged host engine tested the genuine
`RAW_SIGMA_SD9_SRGB.X3F` fixture from rawpy commit
`d050d8a1a061d84e8d4eb6bf3f68a22640aeade3`. It failed during open with
`DT_MOBILE_ERROR_DECODE` (7), produced no PNG and preserved the source hash
`9faed9efe3bb53f34534b487da3045f01d58935f7377450097b6063947964109`.
This establishes a current failure, not the result of a hypothetical flag-ON build.
The upstream test README credits rawsamples.ch under CC BY-NC-SA 4.0; the fixture
remains outside Git and the APK. This is also a limitation of the current bundled
desktop integration, not evidence of an Android-only regression.

## Precision and next increments

Imath is absent, so original TIFF half-float16 support guarded by `HAVE_IMATH`
is disabled. The mobile API currently models UINT8, UINT16 and FLOAT32. Half-float
storage needs its own reviewed representation: the original TIFF module uses a
32-bit pipeline even when storing 16-bit floats; UINT16 is not an equivalent.

The XCF increment retains libxcf gitlink `64efecf63ddd952ccf654e479714aaae6f8c5986`.
Its original module and encoder build on the host and ARM64/API 26. A generated
build copy fixes only error cleanup and buffered IO status; the source submodule,
pixel layout and compression algorithms are unchanged. The adapter uses the
original `plugins/imageio/format/xcf/bpp` configuration, restoring its exact prior
value after each export. Write failures preserve the destination.

GIMP 3.0.4 independently loaded real Fusion GPR exports at 192x185 in all three
precisions. Native RGB bytes and embedded sRGB ICC profiles matched the paired
same-settings TIFF outputs exactly, including finite 32-bit floats. This is host
evidence, not Android execution, full-resolution XCF or mask-channel validation.
The separate `NativeXcfExportTest` passed all 18 phases with `requireXcf=true`
on the same official ARM64/API 28 guest under host single-step emulation and
fixed virtual timing. It opened original Fusion GPR, changed native exposure,
exported all three 128x123 XCF/TIFF pairs, closed the session and verified the
unchanged official source hash. GIMP 3.0.4 independently decoded the pulled,
SHA-256-verified Android outputs: complete native RGB bytes and sRGB ICC profiles
matched each paired TIFF exactly for UINT8, UINT16 and FLOAT32. No source EXIF/XMP
parasites were present under the requested source-metadata exclusion policy.
This does not establish full-resolution XCF, mask-channel export, XCF import,
physical-device behavior or performance. The APK source commit is local
`d866419cb5`; build/output hashes are recorded in PORTING_STATUS.md.

Next small dependency candidates are OpenJPEG/WebP. Imath can
precede OpenEXR. These are source-audit candidates, not successful Android ports.
Each needs actual native build, staging/16 KiB checks, original input/edit/export
and independent output decoding before becoming verified coverage.

Relevant original sources are `src/imageio/imageio.c`, `src/imageio/imageio_rawspeed.cc`,
`src/imageio/imageio_libraw.c`, `src/imageio/format/CMakeLists.txt`,
`src/imageio/format/tiff.c` and `src/external/LibRaw-cmake/CMakeLists.txt`.
Bootstrap switches and dependencies live in `build-android-engine.sh` and
`engine/vcpkg.json`; mobile export choices and settings live in
`mobile/core/darktable_mobile_engine.c` and `mobile/core/dt_mobile_export.c`.
