# Android native dependencies

The Android engine uses the `arm64-android` triplet at API 26 and the repository's
NDK 27.2.12479018. `build-android-dependencies.sh` checks out the immutable vcpkg
2025.06.13 release catalog, whose portfiles pin upstream source versions and
SHA-512-check every downloaded archive, then builds from source. The checked-in
overlay triplet sets `VCPKG_CMAKE_SYSTEM_VERSION` so all dependencies are
compiled for the advertised API 26 baseline. It also supplies the canonical
`aarch64-linux-android` host tuple used by Autoconf-based ports, preventing
vcpkg's make helper from falling back to an ARMv7 compiler for ARM64 builds.
Before invoking `vcpkg install`, the dependency script validates the checked-in
triplet and exports the NDK roots, vcpkg root/default triplet, vcpkg CMake
toolchain, ABI, architecture, and platform. The validation fails unless the
triplet resolves to `arm64-v8a`, `aarch64-linux-android`, and API 26, preventing
a cache or inherited environment from silently changing the target.

The dependency script and Gradle CMake invocation both pin `ANDROID_ABI` to
`arm64-v8a`, `ANDROID_PLATFORM` to `android-26`, and the CMake Android
architecture to `aarch64`; no port-specific ABI override is used. Nothing from
`.vcpkg/` is checked in. Gradle runs this same command before CMake and CI caches
only its installed output and download cache.

| Dependency | Pinned source | Link | License | Upstream reason |
|---|---|---|---|---|
| RawSpeed | repository gitlink `7cf3dc3b9d9c82b414198b1f57460478be6c6c9d` | static | LGPL-2.1-or-later | primary RAW parser/decoder, including GoPro VC-5, and camera metadata |
| Exiv2 | vcpkg 2025.06.13 `exiv2` port | static | GPL-2.0-or-later | EXIF/XMP and ISO-BMFF/CR3 metadata |
| GLib | vcpkg 2025.06.13 `glib` port | static | LGPL-2.1-or-later | upstream core containers, threads, I/O and module abstractions |
| SQLite | vcpkg 2025.06.13 `sqlite3` port | static | public domain | darktable image/history database |
| Little CMS 2 | vcpkg 2025.06.13 `lcms` port | static | MIT | input/output color transforms |
| zlib | vcpkg 2025.06.13 `zlib` port | static | Zlib | compressed metadata and codec dependency |
| libjpeg-turbo | vcpkg 2025.06.13 `libjpeg-turbo` port | static | BSD-3-Clause, IJG, Zlib | JPEG input and export |
| libpng | vcpkg 2025.06.13 `libpng` port | static | libpng-2.0 | PNG input and export |
| libtiff | vcpkg 2025.06.13 `tiff` port | static | libtiff | TIFF/DNG containers and TIFF export |
| pugixml | vcpkg 2025.06.13 `pugixml` port | static | MIT | RawSpeed camera XML database |
| LibRaw | vcpkg 2025.06.13 `libraw` port | static | LGPL-2.1-only or CDDL-1.0 | upstream fallback for files RawSpeed cannot decode |

The catalog tag is the version pin for every vcpkg port; the precise upstream
version, port revision, source URL and archive SHA-512 are recorded in
`ports/<name>/vcpkg.json` and `portfile.cmake` in that tag. Transitive packages
(notably PCRE2 and libffi for GLib, and codec dependencies selected by the
ports) are locked and checksum-verified by the same catalog. Android's Bionic,
`liblog`, C++ shared runtime, pthread and dynamic loader come from the pinned
NDK rather than being duplicated.

RawSpeed already decodes the VC-5 sensor data in GoPro GPR files directly.
The additional official SDK selected by `DT_MOBILE_WITH_GPR_SDK` is an independent
reference decoder and optional conversion utility, downloaded with an immutable
revision and verified archive hash. It is not needed for the normal GPR path.
See [GPR.md](GPR.md) for the direct decoder tests, reference source pin and
optional SDK distribution requirements. The app advertises GPR capability only
when its processing engine and the native decoder are linked.

The third-party libraries in the vcpkg table above are linked into
`libdt_mobile.so` statically; the NDK's `libc++_shared.so` supplies their C++
runtime. When the optional SDK helper is included, `libdt_mobile_gpr.so`
must also be packaged by AGP. It isolates the SDK's bundled Adobe XMP/Expat
from the versions inside Exiv2 and exports only the two GPR cache bridge
functions. The Android engine integration is not enabled or package-verified
yet; builds that include the SDK helper must check all three runtime libraries
on a device. The ordinary RawSpeed path does not need that helper library.
LGPL replacement/relinking and
GPL corresponding-source obligations apply to distributed APKs; the production
workflow's recursive source archive and the notices at repository root must be
distributed with releases.

RawSpeed's `cameras.xml`, plus darktable's `noiseprofiles.json` and
`wb_presets.json`, are copied to generated Android assets. Only those runtime
databases are packaged—not the desktop data tree. Gradle registers a generated
assets producer for each Android variant to make the copy deterministic.

## Direct build and verification

```sh
export ANDROID_NDK_HOME="$ANDROID_SDK_ROOT/ndk/27.2.12479018"
mobile/dependencies/build-android-dependencies.sh
./android/gradlew -p android testDevDebugUnitTest assembleDevRelease
unzip -l android/app/build/outputs/apk/dev/release/app-dev-release.apk
```

Host API tests intentionally do not require this Android-only bundle. The CI
verification additionally uses `llvm-readelf -d` on the unpacked library and
checks that the three databases and `libc++_shared.so` are present.

The separate [Android GTK dependency probe](GTK_PROBE.md) resolves and builds
the Broadway-only GTK closure, then links every GTK/GDK archive object against
Bionic. It is an optional porting check; it does not enable the production
engine or verify an Android editing session.

The experimental [editor engine bootstrap](ENGINE_BOOTSTRAP.md) extends that
route to the root core/module/JNI graph with a separate manifest and output
directory. Its source and Android runtime verification are still incomplete;
it does not enable the production Gradle engine.
Unlike the standalone bundle, that route uses the desktop gitlink's original
LibRaw 0.22.0 sources rather than the catalog's older LibRaw 0.21.4. Its JPEG 8
dependency preserves the original lossy-DNG fallback path, and its OpenMP
runtime also needs Android packaging verification.

The offline [runtime staging tool](STAGING.md) validates a complete installed
Android engine and prepares the full data tree plus a flat native-library
closure. It checks the configured module manifest, AArch64 ELF dependencies,
library collisions and runtime asset contents. Full engine staging and device
loading remain unverified while the complete native build is blocked.
