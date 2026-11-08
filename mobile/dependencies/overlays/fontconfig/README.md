# Fontconfig source mirror

This overlay copies the Fontconfig 2.15.0#4 port from vcpkg `2025.06.13`,
selecting the upstream `fontconfig/fontconfig` GitHub mirror at tag `2.15.0`.
Its archive is byte-identical to the catalog's GitLab archive, retaining
SHA-512
`daa6d1e6058e12c694d9e1512e09be957ff7f3fa375246b9d13eb0a8cf2f21e1512a5cabe93f270e96790e2c20420bf7422d213e43ab9749da3255286ea65a7c`.

The version, features, patches, Meson options and installed data are unchanged.
The experimental engine and GTK probe select this overlay; the standalone
dependency probe continues to use the catalog. Downloads retain the inherited
proxy, certificate verification and destination policy.

This addresses source availability when `gitlab.freedesktop.org` is outside an
executor's allowlist. Fontconfig configuration, font discovery and text/SVG
rendering still need Android runtime integration and verification; a successful
source download or library build does not establish those capabilities.
