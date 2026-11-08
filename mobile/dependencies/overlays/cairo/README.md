# Cairo source mirror for the GTK probe

This optional overlay retains Cairo **1.18.4**, all catalog patches, dependencies,
build options, and install steps from vcpkg **2025.06.13**. Its revision is raised
to 1 to identify the source change. It is used by the GTK probe and experimental
editor engine bootstrap. The default Android bundle still uses the catalog port.

The catalog's GitLab tag archive has SHA-512
`663e6edf2718e8205e30ba309ac609ced9e88e6e1ec857fc48b345dfce82b044d58ec6b4a2d2b281fba30a659a368625ea7501f8b43fe26c137a7ebffdbaac91`.
That host is denied by the managed environment's network policy.

The overlay selects the same version's upstream release tarball, available from
the official Cairo release site and mirrored unchanged in Debian's source pool
as `cairo_1.18.4.orig.tar.xz`. It is not byte-identical to the GitLab archive.
Its SHA-512 is
`863679f817ed67dc2c916c035d740916e27e7e69c04fca63936e37d274e7f4c79848d16c8f7c481798864602e8847c489f698df89b785cbc576c925dbd513316`.
Its SHA-256,
`445ed8208a6e4823de1226a74ca319d3600e83f6369f99b14265006599c32ccb`,
was also checked against Debian's `cairo_1.18.4-1.dsc` source manifest.
