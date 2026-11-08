# Pixman source mirror for the GTK probe

This optional overlay retains Pixman **0.44.2**, both catalog patches,
dependencies, build options, and install steps from vcpkg **2025.06.13**. Its
revision is raised to 1 to identify the source change. It is used by the GTK
probe and experimental editor engine bootstrap. The default Android bundle
still uses the catalog port.

The catalog's GitLab tag archive has SHA-512
`a878d866fbd4d609fabac6a5acac4d0a5ffd0226d926c09d3557261b770f1ad85b2f2d90a48b7621ad20654e52ecccbca9f1a57a36bd5e58ecbe59cca9e3f25d`.
That host is denied by the managed environment's network policy.

The overlay selects the same version's upstream release tarball, mirrored by
Meson's official WrapDB release `pixman_0.44.2-1`. It is not byte-identical to
the GitLab archive. Its SHA-512 is
`5d599568025403c0f5a8499057bbe22ff6bf769459d37c77e2978efd91fd46f9d64cf51d150371a55c6b2f323cc598fdf59649fe050a4a6cf5196a547af3faf1`.
Its SHA-256,
`6349061ce1a338ab6952b92194d1b0377472244208d47ff25bef86fc71973466`,
matches WrapDB's source manifest at that release and Homebrew's 0.44.2 formula,
which both identify the upstream Cairo release URL.
