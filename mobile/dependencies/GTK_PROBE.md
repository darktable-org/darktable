# Android GTK dependency probe

`build-android-gtk-probe.sh` is an optional dependency and linker check for the
headless desktop core port. It has a separate manifest, installed directory,
and build directory; the normal Android dependency bundle and Gradle engine
setting are unchanged.

```sh
export ANDROID_NDK_HOME="$ANDROID_SDK_ROOT/ndk/27.2.12479018"
mobile/dependencies/build-android-gtk-probe.sh --dry-run
mobile/dependencies/build-android-gtk-probe.sh
```

The probe uses vcpkg 2025.06.13, the checked-in static/PIC ARM64 triplet, and API
26. Override `DT_MOBILE_VCPKG_ROOT`, `DT_MOBILE_VCPKG_INSTALLED`, or
`DT_MOBILE_GTK_PROBE_BUILD` to keep an existing checkout or choose other output
directories. `VCPKG_DOWNLOADS` and `VCPKG_DEFAULT_BINARY_CACHE` select caches.
The probe retains build logs and downloaded archives; set
`DT_MOBILE_VCPKG_CLEAN_AFTER_BUILD=1` to request the normal driver's cleanup.
The dependency driver also accepts an optional manifest and port overlay through
`DT_MOBILE_VCPKG_MANIFEST_ROOT` and `DT_MOBILE_VCPKG_OVERLAY_PORTS`; their defaults
preserve the normal bundle.

The GTK overlay retains the catalog's GTK 3.24.43 source SHA-512 and its two
patches. On Android it enables only the Broadway backend, disables X11 and
Wayland, and uses the file print backend. Tests, demonstrations, documentation,
and introspection stay disabled. It supplies real GTK/GDK libraries without
opening a display. Neither the probe nor the intended core integration starts
`broadwayd`, a browser, or a desktop image browser.

The probe also selects explicit source overlays for dependencies whose original
download hosts are unavailable in this build environment. They retain the
catalog versions and build options, but use separately verified upstream source
archives; their hashes are not presented as the original archive hashes:

| Dependency | Source used by the optional probe | Provenance |
| --- | --- | --- |
| GLib 2.84.2 | GNOME's official GitHub commit archive, with its exact gvdb gitlink restored from a second pinned archive | [GLib overlay](overlays/glib/README.md) |
| Cairo 1.18.4 | Upstream release tarball mirrored unchanged in Debian's source pool | [Cairo overlay](overlays/cairo/README.md) |
| Pixman 0.44.2 | Upstream release tarball mirrored by Meson's official WrapDB release | [Pixman overlay](overlays/pixman/README.md) |

The [bzip2 overlay](overlays/bzip2/README.md) additionally supplies Debian's
official source-pool mirror as a fallback URL. Unlike the archive substitutions
above, that mirrored release passes the catalog's original SHA-512 unchanged;
the original patch, CMake build and options are retained. The
[FreeType overlay](overlays/freetype/README.md) uses FreeType's official GitHub
`VER-2-13-3` tag archive instead of `gitlab.freedesktop.org`; its original
catalog SHA-512 also matches unchanged, with the original patches and build
options retained. The [Fontconfig overlay](overlays/fontconfig/README.md) likewise
uses the upstream GitHub `2.15.0` archive with the unchanged catalog SHA-512.
These mirrors still use the configured proxy, TLS checks and HTTP destination
policy.

These overlays are selected by the separate GTK probe and experimental
[engine bootstrap](ENGINE_BOOTSTRAP.md). The default dependency bundle continues
to use the pinned vcpkg catalog ports.

Bionic has no `shm_open` or `shm_unlink`. The additional Android patch selects
GTK's existing temporary-file/mmap transport on both ends. It also keeps the
fallback filename alive until allocation and mapping finish. It does not
replace GTK APIs with stubs.

After dependency installation, CMake links every object in the GTK and GDK
archives into an Android shared library with undefined symbols disallowed.
This catches Bionic APIs hidden in otherwise unused archive members. The
script then checks the ELF machine type and rejects unresolved POSIX shared
memory, X11, or Wayland entry points. `elf-report.txt` is retained beside
`libdt_mobile_gtk_probe.so`.

This is a build/link check. It does not prove that the core can initialize,
process RAW files, export images, or run inside the APK. The exported
`dt_mobile_gtk_probe()` entry point permits a later Android runtime check
without `gtk_init()` or a display. Keep the production engine disabled until
the actual core, modules, resources, and JNI entry points pass Android runtime
verification.

When an upstream download host is unavailable, the vcpkg download cache can be
seeded from a mirror only when its archive passes the original port SHA-512.
For example, GNOME's official GitHub GTK 3.24.43 tag archive is byte-identical
to the catalog's GitLab archive. A different archive or source hash requires a
separate, explicit source change; it must not be relabeled as the pinned file.
