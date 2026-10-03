#!/usr/bin/env python3
"""Check that every iop can safely use the default pixelpipe data allocation.

dt_iop_module_t modules that do not provide their own init_pipe() get
default_init_pipe() (src/develop/imageop.c), which allocates piece->data as

    calloc(1, self->params_size)

that is, by the size of the module's *params* struct. Modules then use
piece->data as their *data* struct. That is only sound while

    sizeof(dt_iop_<op>_data_t) <= sizeof(dt_iop_<op>_params_t)

and nothing in the build enforces it. When a module grows a data struct larger
than its params struct - an extra precomputed field is enough - commit_params()
writes past the end of the allocation and process() reads it back, silently, on
every pixelpipe run. src/iop/contrastntexture.c did exactly this.

The two sizes are never both visible to the compiler in one place: params_size
is a runtime field and the data struct is module-local, so this cannot be a
static_assert in shared code. They are both in the DWARF of the built plugin,
which is what this script reads, via tools/dwarf_types.py.

Usage:
    tools/check-iop-pipe-data-sizes.py [build-dir]

Exits non-zero if any module would overflow.
"""

import glob
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from dwarf_types import TypeSizes # noqa: E402

INTROSPECTION = re.compile(
    r"DT_MODULE_INTROSPECTION\(\s*\d+\s*,\s*(dt_iop_[A-Za-z0-9_]+_params_t)\s*\)")
# A module that defines init_pipe allocates piece->data itself and is exempt.
OWN_INIT_PIPE = re.compile(r"^\s*void\s+init_pipe\s*\(", re.MULTILINE)

# The type a module actually reads piece->data as. Most follow the
# dt_iop_<op>_data_t convention, but not all -- blurs and primaries use their
# params struct directly -- so take the name from the code rather than assume.
PIECE_DATA_USES = (
    re.compile(r"(dt_[A-Za-z0-9_]+_t)\s*\*(?:\s*const)?\s*[A-Za-z0-9_]+\s*=\s*piece->data"),
    re.compile(r"\(\s*(dt_[A-Za-z0-9_]+_t)\s*\*\s*\)\s*piece->data"),
)


def plugin_index(build_dir):
    """Map plugin basename -> path, by searching the tree once.

    The usual location is <build>/lib/darktable/plugins, but searching keeps
    this working for other layouts instead of silently finding nothing.
    """
    index = {}
    for root, dirs, names in os.walk(build_dir):
        dirs[:] = [d for d in dirs if d != "CMakeFiles"]
        for name in names:
            if name.startswith("lib") and name.endswith(".so"):
                index.setdefault(name[3:-3], os.path.join(root, name))
    return index


def main():
    build_dir = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else "build")
    source_dir = os.path.join(os.path.dirname(os.path.abspath(__file__)), os.pardir)

    if not os.path.isdir(build_dir):
        sys.stderr.write(
            "error: no such build directory: %s\n"
            "usage: %s [build-dir]   (needs a tree built with debug info)\n"
            % (build_dir, os.path.basename(sys.argv[0])))
        return 2

    print("source: %s" % os.path.normpath(source_dir))
    print("build : %s\n" % build_dir)

    plugins = plugin_index(build_dir)
    if not plugins:
        sys.stderr.write(
            "error: no plugins under %s -- that is not a built darktable tree.\n"
            "       Pass the build directory, e.g. build-sanitize.\n" % build_dir)
        return 2

    violations, checked, skipped = [], 0, []
    exempt = 0

    for path in sorted(glob.glob(os.path.join(source_dir, "src/iop/*.c"))):
        module = os.path.basename(path)[:-2]
        text = open(path, errors="replace").read()

        params_match = INTROSPECTION.search(text)
        if not params_match:
            continue                      # not an introspected pixelpipe module
        if OWN_INIT_PIPE.search(text):
            exempt += 1                   # allocates piece->data itself
            continue

        params_type = params_match.group(1)

        # Every distinct type this module reads piece->data as, plus the
        # conventional name in case the module only uses it indirectly.
        candidates = {params_type[: -len("_params_t")] + "_data_t"}
        for pattern in PIECE_DATA_USES:
            candidates.update(pattern.findall(text))
        candidates.discard(params_type)   # using params directly is always safe

        plugin = plugins.get(module)
        if not plugin:
            # Example modules such as useless.c are not built by default.
            skipped.append("%s (not built)" % module)
            continue

        types = TypeSizes(plugin)
        params_size = types.size(params_type)
        if params_size is None:
            skipped.append("%s (no debug info for %s -- build with -g?)"
                           % (module, params_type))
            continue

        resolved = {t: types.size(t) for t in sorted(candidates)}
        resolved = {t: n for t, n in resolved.items() if n is not None}
        if not resolved:
            # Reads piece->data as its params struct, or not at all. Safe.
            skipped.append("%s (no pipe data struct of its own)" % module)
            continue

        checked += 1
        for data_type, data_size in sorted(resolved.items()):
            if data_size > params_size:
                violations.append((module, data_type, data_size,
                                   params_type, params_size))

    for module, dtype, dsize, ptype, psize in violations:
        print("%s: %s is %d bytes but piece->data is allocated as %d "
              "(sizeof %s)" % (module, dtype, dsize, psize, ptype))
        print("    add an init_pipe()/cleanup_pipe() pair allocating %s,"
              % dtype)
        print("    as src/iop/toneequal.c does.")

    print("%d module(s) checked, %d overflowing "
          "(%d exempt: own init_pipe, %d skipped)"
          % (checked, len(violations), exempt, len(skipped)))
    if skipped and os.environ.get("VERBOSE"):
        for entry in skipped:
            print("  skipped: %s" % entry)

    # Checking nothing is not a clean result, it is no result. Say so rather
    # than exiting 0, which reads as "nothing to fix".
    sys.stdout.flush()
    if not checked:
        sys.stderr.write(
            "\nerror: no module could be checked. Either %s is not a darktable\n"
            "       build, or its plugins carry no debug info -- build with\n"
            "       -DCMAKE_BUILD_TYPE=RelWithDebInfo.\n" % build_dir)
        return 2

    return 1 if violations else 0


if __name__ == "__main__":
    sys.exit(main())
