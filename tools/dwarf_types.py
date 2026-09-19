"""Read C type sizes out of a build's DWARF, using only binutils objdump.

Why not gdb: it resolves types through a lookup scope, which cannot reach the
function-local typedefs iop modules use for their legacy parameter versions
("No symbol ... in current context"). objdump dumps every DIE regardless of
scope, so those are readable.

Why not pyelftools: objdump ships with binutils, which is already needed to
build darktable, and following DW_AT_type by offset is all the resolution
required here -- no new dependency for the callers.

Needs a build with debug info; RelWithDebInfo is enough.
"""

import re
import subprocess

_DIE = re.compile(r"^\s*<\d+><([0-9a-f]+)>:.*\((DW_TAG_\w+)\)")
# The name is either an indirect string ("...offset: 0x1234): foo") or inline.
_NAME = re.compile(r"DW_AT_name\s*:\s*(?:\(indirect string[^)]*\):\s*)?(\S+)\s*$")
_SIZE = re.compile(r"DW_AT_byte_size\s*:\s*(\d+)")
_TYPE = re.compile(r"DW_AT_type\s*:\s*<0x([0-9a-f]+)>")

# A named struct carries its own size; a typedef of an anonymous struct does
# not, and has to be followed. Both spellings occur across the iop modules.
_CARRIERS = ("DW_TAG_structure_type", "DW_TAG_typedef",
             "DW_TAG_const_type", "DW_TAG_volatile_type")

MAX_CHAIN = 16


class TypeSizes:
    """sizeof() for the named types in one shared object."""

    def __init__(self, shared_object):
        self.path = shared_object
        self._dies = {}
        self._by_name = {}
        self._parse()

    def _parse(self):
        try:
            dump = subprocess.run(["objdump", "--dwarf=info", self.path],
                                  capture_output=True, text=True,
                                  timeout=600).stdout
        except (OSError, subprocess.SubprocessError):
            return

        current = None
        for line in dump.splitlines():
            header = _DIE.match(line)
            if header:
                current = {"tag": header.group(2), "name": None,
                           "size": None, "type": None}
                self._dies[int(header.group(1), 16)] = current
                continue
            if current is None:
                continue
            name = _NAME.search(line)
            if name:
                current["name"] = name.group(1)
                # Several DIEs can share a name (a struct tag and its typedef);
                # keep the first that can yield a size.
                self._by_name.setdefault(name.group(1), []).append(current)
            size = _SIZE.search(line)
            if size:
                current["size"] = int(size.group(1))
            ref = _TYPE.search(line)
            if ref:
                current["type"] = int(ref.group(1), 16)

    def _resolve(self, die):
        """Follow typedef and cv-qualifier chains to whatever carries a size."""
        seen = 0
        while die is not None and seen < MAX_CHAIN:
            if die["size"] is not None:
                return die["size"]
            if die["type"] is None:
                return None
            die = self._dies.get(die["type"])
            seen += 1
        return None

    def size(self, type_name):
        """sizeof(type_name), or None if it is not in this object's DWARF."""
        for die in self._by_name.get(type_name, ()):
            if die["tag"] in _CARRIERS:
                resolved = self._resolve(die)
                if resolved is not None:
                    return resolved
        return None

    def matching(self, pattern):
        """{name: size} for every type whose name matches a compiled regex."""
        found = {}
        for name in self._by_name:
            if pattern.match(name):
                size = self.size(name)
                if size is not None:
                    found[name] = size
        return found

    def usable(self):
        """False when the object carries no debug info worth reading."""
        return bool(self._dies)
