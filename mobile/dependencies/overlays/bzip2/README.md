# bzip2 1.0.8 source mirror overlay

This copies the bzip2 port from vcpkg tag `2025.06.13`, commit
`ef7dbf94b9198bc58f45951adcf1f041fcbc5ea0` (port version 6).
The version, original source SHA-512, patch, CMake build, features and install
options are unchanged. The only port change adds Debian's official source pool
as a download fallback:

https://deb.debian.org/debian/pool/main/b/bzip2/bzip2_1.0.8.orig.tar.gz

This is the original upstream release archive mirrored under Debian's source
filename. Its bytes match the catalog's `bzip2-1.0.8.tar.gz` SHA-512:

`083f5e675d73f3233c7930ebe20425a533feedeaaa9d8cc86831312a6581cefbe6ed0d08d2fa89be81082f2a5abdabca8b3c080bf97218a1bd59dc118a30b9f3`

The mirror was downloaded through the configured proxy with TLS verification
enabled, passed this original hash, and supplied a successful ARM64/API 26
vcpkg bzip2 build in the resumed environment. This is a source/build check;
Android execution and the complete GTK/core/module runtime still require their
own verification.

The separate GTK probe and experimental engine scripts already select this
overlay directory. The default Android dependency bundle continues to use the
catalog port. Future probe/engine builds can download this identical archive
without manually seeding the vcpkg cache when the upstream hosts are unavailable
under the configured network policy.
