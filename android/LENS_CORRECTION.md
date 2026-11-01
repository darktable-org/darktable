<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
# Lens correction

The lens editor reports the correction method, camera/lens match and the
corrections available for the current image and settings. A visible slider must
be consumed by that method. Embedded-metadata controls are not offered for a
file without recognized correction data, and Lensfun controls require a usable
profile. Missing profiles or shooting settings are reported in the panel.
The camera name read from EXIF is shown separately from an actual database
match. An unmatched name is never presented as a selected camera profile.
The normal lens picker contains only profiles compatible with a matched body;
it does not fall back to the entire database when the camera is unknown.
Legacy manual settings remain identifiable as unverified. They may still
affect the preview; **use image lens defaults** removes such an old selection.
Automatic image defaults do not select a lens from an unknown camera's EXIF
name. This guard runs before enabling, editing or initially rendering an active
automatic lens module; it leaves the source image and its EXIF unchanged.

Two native state problems also needed correction. The desktop initializes lens
parameters from image-specific defaults before marking them as modified; the
earlier generic mobile setter could mark an uninitialized parameter blob first,
preserving missing camera, lens or shooting values. The bridge now resolves the
same effective defaults before editing. In addition, the original Lensfun
commit left its processing crop factor at zero when the body did not match,
even with a selected lens and valid image/user crop factor. It now retains that
crop factor as a fallback, with a matched camera still taking precedence.

## Using the editor

1. Enable lens correction with the tool's power switch.
2. Choose **embedded metadata**, **Lensfun database** or **only manual vignette**
   from the methods the native engine offers for this image.
3. For Lensfun, select the matching camera and lens from the searchable native
   catalog. The lens list follows the selected body's compatible profiles. Check
   focal length, aperture and focus distance against the source image. Editable
   initial values do not establish shooting information missing from EXIF.
   Camera and lens names do not need to be identical: compatibility comes from
   the database's mount/crop information, not a string-name comparison.
4. Adjust only the available corrections. Lensfun's automatic scale uses its
   original modifier; **use image lens defaults** restores the image-specific
   starting settings. Both operations use native history and Undo/Redo.

Manual vignette strength remains available without a Lensfun match. Its radius
and steepness become relevant after increasing strength. This is the original
manual adjustment, independent of calibrated optical vignetting correction.
Manual Lensfun chromatic-aberration coefficients require a usable lens profile
and the corresponding override mode.

## Bundled database and GoPro models

The mobile runtime uses Lensfun 0.3.4 with a separately updated official
calibration database, pinned to
[`bc5615d6278cb5fb75dd53e74015367453d5591d` (2026-10-28)](https://github.com/lensfun/lensfun/tree/bc5615d6278cb5fb75dd53e74015367453d5591d/data/db).
The dependency overlay converts that source to database schema 1 using
Lensfun's own `From2To1` converter, and replaces the old XML rather than mixing
database generations. The installed provenance records the source pin and
archive checksum. This updates calibration data without changing the library
ABI. A desktop installation can have newer database files than its library
release; the older mobile bundle had omitted HERO10 and HERO11 for that reason.

The updated action-camera catalog includes `Hero10 black` with
`HERO10 Black & compatibles`, and `Hero11 Black` with
`HERO11 Black & compatibles`. Both pairs provide distortion calibration.
It still contains no HERO12 Black profile. The application must not silently
substitute a HERO4/HERO5/HERO10/HERO11 calibration or present it as verified
HERO12 support. A profile's `& compatibles` suffix does not establish that
HERO12 is calibrated.

## GoPro HERO12 Black embedded data

GPR/DNG correction data has distinct purposes:

| Source data | Processing route | What it establishes |
| --- | --- | --- |
| OpcodeList2 GainMaps | RAW preparation before demosaicing | Per-channel shading correction; not the lens tool's calibrated-vignette slider |
| OpcodeList3 WarpRectilinear | Recognized embedded lens correction | Geometry and/or chromatic correction according to the actual coefficients |
| OpcodeList3 VignetteRadial | Recognized embedded lens correction | Radial vignetting correction when present |

The pinned [official GoPro SDK](https://github.com/gopro/gpr/blob/446c736a38fb14f51343605c0780d347dc602f89/source/lib/gpr_sdk/private/gpr.cpp)
writes GainMaps and a three-plane warp for chromatic aberration. A CA-only warp
does not establish fisheye distortion correction. The editor classifies the
actual recognized source coefficients and shows the available correction, rather
than promising geometry from the mere presence of an opcode list.

An actual HERO12 GPR is still needed to establish which correction data that
file contains. Full geometric correction requires appropriate calibration or
verified geometric coefficients. Recognized embedded TCA and manual vignette
remain usable where applicable; unsupported opcodes or absent calibration do
not become functional by exposing more sliders. The current opcode reader does
not implement WarpFisheye.

## Runtime and verification

The packaged runtime keeps `data/` and `lensfun/version_1/` as siblings; the
original lens module resolves the database relative to the data directory.
The audited database loaded through that route with the original Lensfun
library. A database path failure is reported separately from an unmatched lens.

The implementation uses the original [lens processor](../src/iop/lens.cc) and
[DNG opcode reader](../src/common/dng_opcode.c), not replacement image formulas.
See [native verification](NATIVE_ENGINE_TESTS.md) for the exact fixtures and
platforms tested. Pixel changes on a generated fixture with an installed Canon
profile verify that processing path; they do not verify HERO12 geometry or
physical-device execution.
