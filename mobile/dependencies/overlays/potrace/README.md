# Potrace 1.16 library port

The vcpkg 2025.06.13 catalog has no Potrace port. This overlay builds the real
upstream library required by darktable's bitmap-to-path mask operations. It
does not build the Potrace command line programs or duplicate their tracing
algorithm. The four C sources match `libsources` in upstream `src/Makefile.am`.

The source is the unmodified upstream 1.16 release, preserved by Debian at:

https://deb.debian.org/debian/pool/main/p/potrace/potrace_1.16.orig.tar.gz

Archive size: 657314 bytes. SHA-512:

`10d85ed3fada80951223c65e923b5e6ee3e76e071575971d5b09b996b477ec5ff2403e0337c44963c6b441cc3ba6c4f7009541124a25c16b7721fdebd1f774b9`

Its SHA-256, `be8248a17dedd6ccbaab2fcc45835bb0502d062e40fbded3bc56028ce5eb7acc`,
was checked against Debian's
[`potrace_1.16-2.dsc`](https://deb.debian.org/debian/pool/main/p/potrace/potrace_1.16-2.dsc).
This is a checksum/provenance comparison, not a claim that the Debian OpenPGP
signature was verified.

The wrapper checks `inttypes.h`/`uint64_t` with the selected compiler, supplies
the 1.16 version string, and retains upstream's conservative GCC-compatible
compiler workaround. The library derives its bitmap word size from `unsigned
long`; no host ABI size is substituted for Android's. It uses C99, PIC and
libm, without fast-math. Zlib and getopt are CLI-only dependencies.

The installed interface is `potracelib.h`, `libpotrace.a`, pkg-config module
`potrace`, and CMake target `potrace::potrace`. Darktable's existing
`FindPotrace.cmake` can continue to locate the header/archive and provide its
`Potrace::Potrace` target.

Potrace is GPL-2.0-or-later (the upstream `README` and `src/main.c` state
version 2 or any later version). Source copyright notices are retained and
`COPYING`/`AUTHORS` are installed in the package copyright file. Distributed
builds must include the corresponding source under the project's existing
GPL distribution process.

For a direct host fixture or Android link check, configure this directory with
`POTRACE_SOURCE_DIR` pointing to the extracted original release and
`POTRACE_BUILD_PROBE=ON`. On a host it traces an empty bitmap and a rectangle,
checking path count, sign, area, four corners and unmodified input. On Android
it builds a shared library containing every archive object with undefined
symbols forbidden. The exported `potrace_android_trace_probe()` uses the same
fixture, but still needs execution on Android before claiming device support.

The recipe was validated with vcpkg 2025.06.13, NDK 27.2.12479018, ARM64 and
API 26. Both installed Debug and Release archives contain AArch64 objects.
Separate installed-package shared-library consumers linked the complete
archive with `--no-undefined` through both the exported CMake target and
darktable's existing `FindPotrace.cmake`. The host fixture passed; these
Android checks verify compilation and linking, not device execution.
