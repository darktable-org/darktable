#!/usr/bin/env python3
"""Check each iop's versioned legacy parameter structs against real stored data.

When a module's parameters change, the old layout is preserved as a
dt_iop_<op>_params_v<N>_t struct inside legacy_params(), and history entries
written by older darktable versions are converted through it. Nothing checks
that the struct still matches what those versions actually wrote. When it drifts,
legacy_params() reads a struct's worth of bytes out of a blob that is shorter,
running off the end of the allocation dt_iop_legacy_params() sized from the
stored data. Both src/iop/highlights.c and src/iop/denoiseprofile.c did this.

The two sides are never visible together at compile time -- the on-disk size is
a runtime property of user data -- but both are recoverable afterwards:

  * the struct sizes are in the DWARF of the built plugins, including the
    function-local typedefs, which is why this reads objdump rather than gdb;
  * the sizes actually written are in the history entries of the integration
    test XMPs, which cover a wide range of darktable versions.

Usage:
    tools/check-iop-legacy-params.py <build-dir> [xmp-dir]

xmp-dir defaults to src/tests/integration. Exits 1 on a size mismatch, 2 if
nothing could be checked. Missing structs are reported as warnings only, see
below.
"""

import base64
import collections
import glob
import os
import re
import sys
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from dwarf_types import TypeSizes # noqa: E402

HISTORY_ENTRY = re.compile(r"<rdf:li[^>]*?/>", re.S)
ATTR = {name: re.compile(r'%s="([^"]*)"' % name)
        for name in ("operation", "modversion", "params")}
VERSIONED = re.compile(r"^dt_iop_[a-z0-9_]+_params_v(\d+)_t$")


def decode(payload):
    """Undo dt_exif_xmp_encode: plain hex, or "gz" + factor + base64 + zlib."""
    if payload.startswith("gz"):
        return zlib.decompress(base64.b64decode(payload[4:]))
    return bytes.fromhex(payload)


def stored_sizes(xmp_dir):
    """{(operation, version): {sizes seen}} across every test XMP."""
    corpus = collections.defaultdict(set)
    for path in sorted(glob.glob(os.path.join(xmp_dir, "0*", "*.xmp"))):
        text = open(path, errors="replace").read()
        for entry in HISTORY_ENTRY.finditer(text):
            fields = {k: r.search(entry.group(0)) for k, r in ATTR.items()}
            if not all(fields.values()):
                continue
            try:
                size = len(decode(fields["params"].group(1)))
                version = int(fields["modversion"].group(1))
            except (ValueError, zlib.error, base64.binascii.Error):
                continue
            corpus[(fields["operation"].group(1), version)].add(size)
    return corpus


def plugin_index(build_dir):
    index = {}
    for root, dirs, names in os.walk(build_dir):
        dirs[:] = [d for d in dirs if d != "CMakeFiles"]
        for name in names:
            if name.startswith("lib") and name.endswith(".so"):
                index.setdefault(name[3:-3], os.path.join(root, name))
    return index


def main():
    if not 2 <= len(sys.argv) <= 3:
        sys.stderr.write("usage: %s <build-dir> [xmp-dir]\n"
                         % os.path.basename(sys.argv[0]))
        return 2

    build_dir = os.path.abspath(sys.argv[1])
    source_dir = os.path.join(os.path.dirname(os.path.abspath(__file__)), os.pardir)
    xmp_dir = os.path.abspath(sys.argv[2] if len(sys.argv) > 2
                              else os.path.join(source_dir, "src/tests/integration"))

    if not os.path.isdir(build_dir):
        sys.stderr.write("error: no such build directory: %s\n" % build_dir)
        return 2
    if not os.path.isdir(xmp_dir):
        sys.stderr.write(
            "error: no XMP corpus at %s\n"
            "       git submodule update --init src/tests/integration\n" % xmp_dir)
        return 2

    print("build : %s" % build_dir)
    print("XMPs  : %s\n" % xmp_dir)

    corpus = stored_sizes(xmp_dir)
    if not corpus:
        sys.stderr.write("error: no history entries found under %s\n" % xmp_dir)
        return 2

    plugins = plugin_index(build_dir)
    if not plugins:
        sys.stderr.write("error: no plugins under %s -- not a built tree.\n" % build_dir)
        return 2

    errors, warnings, checked = [], [], 0

    for operation in sorted({op for op, _ in corpus}):
        plugin = plugins.get(operation)
        if not plugin:
            continue
        types = TypeSizes(plugin)
        if not types.usable():
            continue
        compiled = {int(VERSIONED.match(n).group(1)): size
                    for n, size in types.matching(VERSIONED).items()}
        if not compiled:
            continue

        for (op, version), sizes in sorted(corpus.items()):
            if op != operation:
                continue
            checked += 1
            if version in compiled:
                if compiled[version] not in sizes:
                    errors.append(
                        "%s v%d: struct is %d bytes, but stored history is %s"
                        % (op, version, compiled[version],
                           " or ".join(str(s) for s in sorted(sizes))))
            else:
                warnings.append(
                    "%s v%d: %s bytes stored, no _params_v%d_t in debug info"
                    % (op, version, " or ".join(str(s) for s in sorted(sizes)),
                       version))

    for line in errors:
        print("error: %s" % line)
    if errors:
        print("    legacy_params() reads sizeof(struct) from a blob that size,")
        print("    so the conversion runs off the end of the allocation.\n")

    # A local type the compiler had no reason to emit is indistinguishable here
    # from one that was never written, so this half cannot be an error. It still
    # points at real defects: denoiseprofile's v10 struct was #if 0'd out, which
    # is what sent its conversion through the current struct instead.
    for line in warnings:
        print("warning: %s" % line)

    print("\n%d (operation, version) pair(s) checked, %d error(s), %d warning(s)"
          % (checked, len(errors), len(warnings)))

    if not checked:
        sys.stderr.write("\nerror: nothing could be checked -- no plugin in %s\n"
                         "       matched an operation in the corpus, or the\n"
                         "       plugins carry no debug info.\n" % build_dir)
        return 2
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
