# GLib source mirror for the GTK probe

This optional overlay keeps GLib **2.84.2**, the dependency graph, build options,
installation steps, and both patches from the vcpkg **2025.06.13** catalog. It
is selected by the GTK dependency probe and experimental editor engine
bootstrap. The normal Android bundle still uses the catalog port.

The catalog downloads `glib-2.84.2.tar.xz` from `download.gnome.org`, including
the gvdb submodule. Its SHA-512 is
`430928d7d7a442fc3927ca943f2569035fe8768768a0ebc6720ae1ef152b56fc5f8d4215d21b4828cc2f39a8632c907ed2c52a0c8566da1c533a2e049a1a121f`.
That download host is denied by the managed environment's network policy.

The overlay instead checksums archives from GNOME's official GitHub mirrors.
These are Git source archives, not byte-identical copies of the release tarball;
their independent hashes are recorded honestly in `portfile.cmake`.

| Source | Immutable revision | SHA-512 |
| --- | --- | --- |
| `GNOME/glib` | `2371bee17d85318480b3ddeeab4f5107b4889ad7` | `675b9f1d8fa225ac6062cbeb621584c7f257a5739a0a78901e17ba851dea544411a8fb5901727bcbac12a7ab539e23d2a51192f47d55bab66fe76fd95777e1ed` |
| `GNOME/gvdb` | `2b42fc75f09dbe1cd1057580b5782b08f2dcb400` | `3fa37107c7f6a31d03c2b2c8d0878fb6ca35ee4ad8854136cdfdefecc9a28d307ef8f404ced369176a22b6bd7eec7ca637e29e48fad1c7bed2912dc853c1f9cf` |

The annotated GLib `2.84.2` tag is
`2dcb481f38844ffc7044e656426a2af0958997cf`; it peels to the GLib revision
above. `git ls-tree` at that revision records the same gvdb gitlink used by
the overlay. The source archive omits gitlink contents, so the separately
verified gvdb archive is copied into `subprojects/gvdb` before configuration.
Meson therefore does not need to fetch that submodule from GNOME GitLab.
