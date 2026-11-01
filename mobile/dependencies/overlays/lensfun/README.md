# Lensfun 0.3.4 Android overlay

This extends the vcpkg 2025.06.13 port to Android ARM64. The upstream tag,
SHA-512 and existing Windows-only build patch are unchanged:

https://github.com/lensfun/lensfun/archive/v0.3.4.tar.gz

SHA-512:
`4db9a08d51ba50c7c2ff528d380bb28e34698b2bb5c40e5f3deeaa5544c888ac7e0f638bbc3f33a4f75dbb67e0425ca36ce6d8cd1d8c043a4173a2df47de08c6`

The catalog's `!arm` restriction prevents dependency resolution before a
compile. Lensfun 0.3.4 already provides complete scalar implementations of
distortion, geometry, chromatic aberration and vignetting correction. Its only
hand-written SIMD paths are optional SSE/SSE2. Every CPU detection call is
guarded by those SIMD macros. This overlay explicitly disables SSE/SSE2 on
ARM64 and enables PIC; it does not substitute a CPU detector or correction
algorithm. There is no upstream NEON path to enable in this release.

The original GLib dependency, library, generated public header and pkg-config
interface are retained. CLI, helper scripts, Python and documentation are not
built by the default Android package. A host-only Python converter updates the
XML calibration database from a separately pinned official revision, keeping
the 0.3.4-compatible schema. See [DATABASE.md](DATABASE.md) for the database
source pin, conversion, calibration checks and licenses. The XML database is
installed under `share/lensfun/version_1`. Android must unpack that data to
app storage. The existing desktop lens module retries loading
`../lensfun/version_1` relative to its configured data directory, so the mobile
runtime tree must retain `data/` and `lensfun/version_1/` as siblings. The default
search path embedded by upstream is not an Android asset path.

Upstream's README identifies the libraries as LGPLv3 and the database as
CC-BY-SA 3.0. Some public source headers retain an earlier LGPL-or-later
notice, so the port retains the catalog's unspecified SPDX expression and
preserves the original notices. GPLv3, LGPLv3 and the database's CC-BY-SA 3.0
texts are included in the package copyright file.

The `probe/` directory is an independent consumer. It parses an in-memory XML
calibration through the real database API and verifies POLY3 correction:
`(225,150)` maps to `(219.375,150)` in a 301-square image, its center stays
fixed, and the inverse maps back within tolerance. Host builds execute this
fixture. Android builds require `LENSFUN_ARCHIVE`, include the complete
archive in a shared library and forbid undefined symbols; their exported
`lensfun_android_correction_probe()` still requires execution on Android.

## Verification scope

The host fixture passed with the pinned 0.3.4 sources and both SIMD options
disabled. NDK 27.2.12479018 compiled the same library as PIC for ARM64/API 26;
all 13 archive objects were checked as AArch64. The consumer linked the complete
archive with `--whole-archive` and `--no-undefined`, exported its probe function,
and depended only on `libc++_shared.so`, `libc.so`, `libm.so` and `libdl.so`.
That original library validation used all 55 XML files from the v0.3.4 source
unchanged. The current overlay instead installs the 56 officially converted
XML files described in [DATABASE.md](DATABASE.md).

That Android link used an isolated validation build of the real GLib 2.84.2
core, with its exact gvdb revision, libffi 3.5.1, and GNU libintl 0.23.1. The
libintl source was the original Debian-hosted release, checked against Debian's
source manifest; it differs from the catalog's 0.22.5 dependency. GLib's
translation-tool generation was disabled for this validation build. No GLib or
Lensfun APIs were replaced with stubs. This establishes a real ARM64 link of
Lensfun, not completion of the formal vcpkg dependency chain or Android runtime
execution. The formal bundle still needs its original gettext 0.22.5 sources.
