# FreeType source mirror

This overlay copies the FreeType 2.13.3 port from vcpkg `2025.06.13` and
selects the upstream project's official `freetype/freetype` GitHub mirror at
`VER-2-13-3`. The GitHub tag archive is byte-identical to the catalog's GitLab
archive: SHA-512 remains
`fccfaa15eb79a105981bf634df34ac9ddf1c53550ec0b334903a1b21f9f8bf5eb2b3f9476e554afa112a0fca58ec85ab212d674dfd853670efec876bacbe8a53`.

The version, features, patches, CMake options and license installation are
unchanged. This is selected only by the experimental engine/GTK overlay
bundle. It does not replace FreeType's font-rendering algorithms or change
the standalone Android dependency bundle.

The mirror is useful when `gitlab.freedesktop.org` is outside the executor's
HTTP allowlist. Downloads still use the inherited proxy, TLS verification,
and destination policy; this overlay grants no network access.
