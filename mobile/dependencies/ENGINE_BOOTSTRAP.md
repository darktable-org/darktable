# Android editor engine bootstrap

`build-android-engine.sh` is the experimental build entry point for the real
desktop processing graph on Android. It builds the core, all enabled image
operation and export modules, runtime data, and JNI adapter without desktop
applications or the image browser. The production Gradle build still has its
processing engine disabled; this script does not change that setting.

The configured ARM64/API 26 graph has now been built, linked, installed and
staged in a fresh environment. An experimental engine-enabled development APK
has also passed packaging and final runtime byte verification. A preserved APK
pair passed the full Fusion GPR JNI edit/undo/export/reopen test on ARM64/API 28
with host single-step emulation and fixed virtual timing. The newer default-instance
APK also passed its full 41-phase run; neither result establishes all formats/features
or physical-device performance.
See the dated [port status](PORTING_STATUS.md)
for the exact configuration, test results and remaining work.

```sh
git submodule update --init --recursive
export ANDROID_NDK_HOME="$ANDROID_SDK_ROOT/ndk/27.2.12479018"
mobile/dependencies/build-android-engine.sh --dry-run
mobile/dependencies/build-android-engine.sh
```

Use CMake 3.22.1 or newer, Ninja, a C/C++ host compiler, Git, Python, Perl,
pkg-config, and the Autotools required by the selected vcpkg ports. The desktop
graph also needs a host `xsltproc` or Saxon XSLT processor for generated files.
Installing Android libxml2 does not supply that executable. vcpkg supplies the
other host build tools declared by the pinned ports.

The bootstrap builds `dt_mobile_runtime`, which includes JNI, the core, all
configured processing/format/storage modules and generated runtime data. When
resuming an already configured build, use that target before installing it:

```sh
cmake --build "$DT_MOBILE_ENGINE_BUILD" --target dt_mobile_runtime --parallel 2
```

The internal `dt_mobile_modules` target builds the dynamically loaded module
set and data without JNI. It is not a complete runtime for APK staging.

The script uses the vcpkg 2025.06.13 catalog, the API 26 ARM64 static/PIC
triplet, and the explicit [port overlays](overlays). Build and installed output
default to `.vcpkg/engine/build` and `.vcpkg/engine/installed`, separate from
the normal Android bundle and the [GTK probe](GTK_PROBE.md). Override
`DT_MOBILE_ENGINE_BUILD`, `DT_MOBILE_VCPKG_INSTALLED`, and
`DT_MOBILE_VCPKG_ROOT` to select other directories. `VCPKG_DOWNLOADS` and
`VCPKG_DEFAULT_BINARY_CACHE` permit reuse of checksum-verified sources and
built packages. Keep concurrent builds in separate vcpkg checkouts as well as
separate installed directories: vcpkg also writes per-port build trees.

`--dry-run` only resolves the dependency graph. It does not produce an
environment file or prove that any library builds. The normal invocation
retains dependency build logs; set `DT_MOBILE_VCPKG_CLEAN_AFTER_BUILD=1` to
request cleanup. `DT_MOBILE_ENGINE_BUILD_TYPE` selects `Release` (default),
`RelWithDebInfo`, or `Debug`.

The engine manifest enables Exiv2's BMFF, PNG, and XMP support and the JPEG 8
API needed by LibRaw's lossy-DNG decoder. RawSpeed remains the primary RAW
decoder, including its original GoPro VC-5 decoder. The fallback uses the
desktop repository's original LibRaw and LibRaw-cmake sources with
`DONT_USE_INTERNAL_LIBRAW=OFF`. The current gitlink
`f74ddd995c5447458f132a5377ca0f4b394dff6e` is LibRaw 0.22.0 Release. The
catalog's LibRaw 0.21.4 has actual decoder differences and does not establish
the same camera support. The normal standalone dependency probe still uses
its separate catalog bundle.
Potrace uses its original library sources through the separately documented
[overlay](overlays/potrace/README.md). The [Lensfun overlay](overlays/lensfun/README.md)
retains the pinned correction algorithms and calibration data, using its
complete scalar implementation on ARM64 with the x86 SIMD paths disabled.

The root CMake build isolates pkg-config to the Android target prefix and
retains the private static dependency chain. The desktop core is linked with
the C++ linker and undefined symbols are disallowed. Processing, format, and
storage modules use their Android library filenames for eventual installation
in the APK's extracted native library directory.

The root engine and standalone Gradle native configurations enable NDK r27's
`ANDROID_SUPPORT_FLEXIBLE_PAGE_SIZES`. This removes Bionic's compile-time
`PAGE_SIZE` macro and links ARM64 shared libraries with 16 KiB `PT_LOAD`
alignment, retaining compatibility with 4 KiB devices. The pinned NDK's ARM64
`libc++_shared.so` and `libomp.so` also have 16 KiB load alignment. Static/PIC
dependency archives retain their existing build configuration; final shared
libraries must be relinked to apply the setting. Inspect every packaged shared
library and validate the APK on both page sizes before claiming device support.

The recorded `90ce43520f` bootstrap disables optional JPEG XL, AVIF/HEIF, JPEG 2000, WebP,
OpenEXR and XCF codecs, G'MIC, AI, OpenCL, Lua, camera tethering, and desktop
integrations. These settings reduce the initial porting graph; they do not
meet the requested desktop feature and format parity. They must be revisited
after the initial Android runtime works.

Source hosts required by the current graph include `ftp.gnu.org`,
`gitlab.gnome.org`, `download.gnome.org`,
`sqlite.org`, and the SourceForge mirror selected for libuuid. Restricted build
environments must
allow those destinations or supply archives that match the source pins.
Preserve the proxy, certificate checks, and original archive hashes when
seeding a download cache.

The [bzip2 overlay](overlays/bzip2/README.md) adds Debian's official source-pool
mirror for the exact pinned 1.0.8 archive. The catalog SHA-512, patch, CMake
build and options are unchanged, allowing reproducible downloads without manual
cache seeding when its upstream hosts are unavailable under the network policy.
The [FreeType overlay](overlays/freetype/README.md) similarly selects the
official GitHub `VER-2-13-3` tag archive with the original catalog SHA-512 and
build settings unchanged when `gitlab.freedesktop.org` is not permitted. The
[Fontconfig overlay](overlays/fontconfig/README.md) selects the corresponding
upstream GitHub `2.15.0` archive, also with the original SHA-512 unchanged.
These overlays do not change the executor's network policy or bypass its proxy.

The original LibRaw archive has passed an ARM64/API 26 whole-archive link
check with DNG deflate, lossy-DNG and OpenMP enabled. That configuration uses
the NDK's shared `libomp.so`, which must be packaged alongside
`libc++_shared.so` if the complete engine selects that runtime. A standalone
archive/link check does not prove decoding or execution in the Android app.

The configured core/modules and JNI now link successfully, and the real stage
includes their checked shared-library closure, runtime data and Lensfun database.
The experimental APK has passed the final runtime-content checks. The separate
API 28 Fusion run above also passed JNI open/edit/undo/export/close/reopen,
including source preservation and default-instance topology. Before enabling
the default Gradle engine, extend that evidence to physical devices, other
original RAW/GPR files, cancellation and lifecycle behavior. Host pipeline tests,
native linking, staging and APK byte verification alone do not establish those
Android operations or complete desktop feature parity.

After a complete native build, use the offline [runtime staging tool](STAGING.md)
to check the configured module manifest, shared-library closure and data before
integrating the result into an APK. It retains native modules in a flat
`jniLibs/arm64-v8a` directory and data in `assets/darktable-runtime`.

The opt-in [engine APK workflow](../../.github/workflows/android-engine.yml)
records these steps without enabling the default Gradle engine. It runs for
native/runtime changes on `feature/android-raw-editor`, with manual dispatch
also available. Its first complete
[remote run at `90ce43520f`](https://github.com/togekk39/darktable-android/actions/runs/40722614711)
passed native build, fresh stripped installation, staging, final APK runtime
hashes, signatures and 16 KiB load alignment, then uploaded both APKs. That
workflow does not execute Android instrumentation. See the
[status notes](PORTING_STATUS.md) for the artifact and separate JNI evidence.
