#!/usr/bin/env python3
"""Stage an installed ARM64 Android engine; never build or rewrite native code."""
# SPDX-License-Identifier: GPL-3.0-or-later
from __future__ import annotations

import argparse
from collections import defaultdict, deque
from dataclasses import dataclass
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import struct
import sys
import tempfile


SYSTEM_LIBRARIES = frozenset({
    "libc.so", "libm.so", "libdl.so", "liblog.so", "libandroid.so", "libz.so",
})
REQUIRED_DATA = ("darktablerc", "rawspeed/cameras.xml", "noiseprofiles.json", "wb_presets.json")
LIBRARY_NAME = re.compile(r"lib[A-Za-z0-9_+.-]+\.so\Z")


class StageError(Exception):
    pass


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def library_name(name: str) -> str:
    if not LIBRARY_NAME.fullmatch(name):
        raise StageError(f"not an APK native-library filename: {name!r}; rebuild with an unversioned .so SONAME")
    return name


@dataclass(frozen=True)
class Elf:
    path: Path
    soname: str | None
    needed: tuple[str, ...]
    search_paths: dict[str, str]


def read_elf(path: Path) -> Elf:
    """Read ELF program/dynamic headers without requiring a host readelf binary."""
    try:
        data = path.read_bytes()
        if len(data) < 64 or data[:7] != b"\x7fELF\x02\x01\x01":
            raise StageError(f"expected an ELF64 little-endian library: {path}")
        header = struct.unpack_from("<HHIQQQIHHHHHH", data, 16)
        elf_type, machine, version = header[:3]
        if elf_type != 3 or machine != 183 or version != 1:
            raise StageError(f"expected an AArch64 ET_DYN library, got type={elf_type}, machine={machine}: {path}")
        phoff, ehsize, phsize, phnum = header[4], header[7], header[8], header[9]
        if ehsize != 64 or phsize != 56 or not phnum or phoff + phsize * phnum > len(data):
            raise StageError(f"invalid ELF program headers: {path}")
        loads, dynamic = [], None
        for index in range(phnum):
            ptype, _, offset, address, _, file_size, _, _ = struct.unpack_from("<IIQQQQQQ", data, phoff + index * phsize)
            if offset + file_size > len(data):
                raise StageError(f"ELF segment extends beyond the file: {path}")
            if ptype == 3:  # PT_INTERP: a PIE executable is not a shared library.
                raise StageError(f"executable interpreter is not allowed in a native library: {path}")
            if ptype == 1:
                loads.append((address, offset, file_size))
            elif ptype == 2:
                if dynamic is not None:
                    raise StageError(f"multiple ELF dynamic segments: {path}")
                dynamic = (offset, file_size)
        if dynamic is None or dynamic[1] % 16:
            raise StageError(f"missing or invalid ELF dynamic segment: {path}")
        entries = []
        terminated = False
        for offset in range(dynamic[0], dynamic[0] + dynamic[1], 16):
            tag, value = struct.unpack_from("<qQ", data, offset)
            if tag == 0:
                terminated = True
                break
            entries.append((tag, value))
        if not terminated:
            raise StageError(f"unterminated ELF dynamic segment: {path}")
        tags = dict(entries)
        if 5 not in tags or 10 not in tags:
            raise StageError(f"missing ELF dynamic string table: {path}")
        table_address, table_size = tags[5], tags[10]
        table_offset = None
        for address, offset, file_size in loads:
            if address <= table_address and table_address + table_size <= address + file_size:
                table_offset = offset + table_address - address
                break
        if table_offset is None:
            raise StageError(f"ELF dynamic string table is outside file-backed segments: {path}")

        def dynamic_string(index: int) -> str:
            if index >= table_size:
                raise StageError(f"ELF string index is outside the table: {path}")
            end = data.find(b"\0", table_offset + index, table_offset + table_size)
            if end < 0:
                raise StageError(f"unterminated ELF dynamic string: {path}")
            try:
                return data[table_offset + index:end].decode("ascii")
            except UnicodeDecodeError as error:
                raise StageError(f"non-ASCII ELF library name: {path}") from error

        needed = tuple(library_name(dynamic_string(value)) for tag, value in entries if tag == 1)
        sonames = [library_name(dynamic_string(value)) for tag, value in entries if tag == 14]
        if len(sonames) > 1:
            raise StageError(f"multiple ELF SONAME entries: {path}")
        search_paths = {}
        for tag, value in entries:
            if tag not in (15, 29):
                continue
            tag_name = "RPATH" if tag == 15 else "RUNPATH"
            if tag_name in search_paths:
                raise StageError(f"multiple ELF {tag_name} entries: {path}")
            search = dynamic_string(value)
            if search and any(part not in ("$ORIGIN", "${ORIGIN}") for part in search.split(":")):
                raise StageError(f"ELF {tag_name} is incompatible with a flat nativeLibraryDir: {search!r}: {path}; relink with no search path or $ORIGIN")
            search_paths[tag_name] = search
        return Elf(path, sonames[0] if sonames else None, needed, search_paths)
    except (OSError, struct.error) as error:
        raise StageError(f"cannot read ELF {path}: {error}") from error


def shared_files(root: Path):
    if not root.is_dir():
        raise StageError(f"library root is not a directory: {root}")
    for directory, directories, filenames in os.walk(root, followlinks=False):
        directories.sort()
        for filename in sorted(filenames):
            if ".so" in filename and (filename.endswith(".so") or ".so." in filename):
                path = Path(directory) / filename
                if not path.is_file():
                    raise StageError(f"invalid shared-library file: {path}")
                yield path


class Closure:
    def __init__(self, roots: list[Path], ndk: Path | None):
        self.candidates: dict[str, list[Path]] = defaultdict(list)
        self.cache: dict[Path, tuple[Elf, str]] = {}
        self.system = set()
        for root in roots:
            for path in shared_files(root):
                self.candidates[path.name].append(path)
        if ndk:
            prebuilts = list((ndk / "toolchains/llvm/prebuilt").glob("*/sysroot"))
            if len(prebuilts) != 1:
                raise StageError("--ndk must contain one LLVM prebuilt host toolchain")
            sysroot = prebuilts[0]
            api = sysroot / "usr/lib/aarch64-linux-android/26"
            for name in SYSTEM_LIBRARIES:
                stub = api / name
                read_elf(stub)
            runtime = sysroot / "usr/lib/aarch64-linux-android/libc++_shared.so"
            if not runtime.is_file():
                raise StageError(f"NDK C++ shared runtime is missing: {runtime}")
            self.candidates[runtime.name].append(runtime)
            for runtime in sorted(sysroot.parent.glob("lib/clang/*/lib/linux/aarch64/libomp.so")):
                self.candidates[runtime.name].append(runtime)

    def inspect(self, path: Path) -> tuple[Elf, str]:
        resolved = path.resolve(strict=True)
        if resolved not in self.cache:
            self.cache[resolved] = read_elf(path), sha256(path)
        return self.cache[resolved]

    def resolve(self, name: str) -> Path:
        paths = self.candidates.get(name, [])
        if not paths:
            raise StageError(f"unresolved DT_NEEDED library: {name}; add its Android directory with --library-root")
        hashes = {self.inspect(path)[1] for path in paths}
        if len(hashes) != 1:
            raise StageError(f"different libraries collide as {name}: " + ", ".join(map(str, paths)))
        return paths[0]

    def collect(self, seeds: list[Path]) -> dict[str, dict]:
        pending = deque(seeds)
        libraries = {}
        while pending:
            path = pending.popleft()
            name = library_name(path.name)
            if name in SYSTEM_LIBRARIES:
                raise StageError(f"system library must not be bundled: {path}")
            elf, digest = self.inspect(path)
            if elf.soname and elf.soname != name:
                raise StageError(f"SONAME {elf.soname} does not match staged filename {name}: {path}")
            if name in libraries:
                if libraries[name]["sha256"] != digest:
                    raise StageError(f"different libraries collide as {name}: {libraries[name]['source']}, {path}")
                continue
            libraries[name] = {"source": str(path), "sha256": digest,
                               "soname": elf.soname, "needed": list(elf.needed),
                               "searchPaths": elf.search_paths}
            for dependency in elf.needed:
                if dependency in SYSTEM_LIBRARIES:
                    self.system.add(dependency)
                else:
                    pending.append(self.resolve(dependency))
        return dict(sorted(libraries.items()))


def data_files(root: Path) -> list[Path]:
    if not root.is_dir():
        raise StageError(f"runtime data directory is missing: {root}")
    files = []
    for directory, directories, filenames in os.walk(root, followlinks=False):
        directories.sort()
        for name in directories + sorted(filenames):
            path = Path(directory) / name
            if "\\" in name or any(ord(character) < 0x20 for character in name):
                raise StageError(f"unsupported runtime asset path component: {path}")
            if path.is_symlink():
                raise StageError(f"runtime data symlinks are not supported; install regular data files: {path}")
        for name in sorted(filenames):
            path = Path(directory) / name
            if not path.is_file():
                raise StageError(f"runtime asset is not a regular file: {path}")
            with path.open("rb") as stream:
                signature = stream.read(4)
            lower_name = name.lower()
            if signature == b"\x7fELF" or lower_name.endswith(".so") or ".so." in lower_name:
                raise StageError(f"native code is forbidden in runtime assets: {path}")
            files.append(path)
    return files


def manifest_seeds(prefix: Path) -> tuple[list[Path], list[str]]:
    manifest = prefix / "share/darktable-mobile/android-runtime-libraries.txt"
    if not manifest.is_file():
        raise StageError(f"missing Android engine library manifest: {manifest}; configure/build/install the current library-only Android engine")
    names = manifest.read_text(encoding="utf-8").splitlines()
    if len(names) != len(set(names)):
        raise StageError(f"duplicate library names in {manifest}")
    for name in names:
        library_name(name)
    if not {"libdarktable.so", "libdt_mobile.so"}.issubset(names):
        raise StageError("Android manifest must contain libdarktable.so and libdt_mobile.so")
    for category in ("iop", "format", "storage"):
        if not any(name.startswith(f"libdt_{category}_") for name in names):
            raise StageError(f"Android manifest has no {category} plugins")
    # Every installed shared library is checked and staged, including modules
    # loaded dynamically. The manifest detects an incomplete installation.
    installed = []
    for root in (prefix / "lib", prefix / "lib64"):
        if root.is_dir():
            installed.extend(shared_files(root))
    available = {path.name for path in installed}
    missing = sorted(set(names) - available)
    if missing:
        raise StageError("installed Android engine libraries are missing: " + ", ".join(missing))
    return installed, names


def translation_files(root: Path) -> list[Path]:
    files = data_files(root)
    if not any(path.relative_to(root).parts[1:] == ("LC_MESSAGES", "darktable.mo")
               and path.stat().st_size > 0 for path in files):
        raise StageError(f"runtime translations contain no nonempty LC_MESSAGES/darktable.mo: {root}")
    return files


def stage(args, closure: Closure):
    prefix = args.prefix.resolve(strict=True)
    output = args.output.absolute()
    if output.exists() or output.is_symlink():
        raise StageError(f"staging output already exists; choose a fresh destination: {output}")
    for source in [prefix, *args.library_root, *([args.lensfun_data] if args.lensfun_data else [])]:
        if output.resolve().is_relative_to(source.resolve()):
            raise StageError("staging output must be outside all input prefixes/library roots")
    seeds, configured_libraries = manifest_seeds(prefix)
    libraries = closure.collect(seeds)
    data = prefix / "share/darktable"
    data_paths = data_files(data)
    for name in REQUIRED_DATA:
        path = data / name
        if not path.is_file() or not path.stat().st_size:
            raise StageError(f"required runtime data is missing or empty: {path}")
    lensfun = args.lensfun_data or prefix / "share/lensfun/version_1"
    lensfun_paths = data_files(lensfun)
    if not any(path.suffix == ".xml" and path.stat().st_size for path in lensfun_paths):
        raise StageError(f"Lensfun version_1 contains no nonempty XML calibration files: {lensfun}")
    asset_sources = [(path, Path("data") / path.relative_to(data)) for path in data_paths]
    asset_sources += [(path, Path("lensfun/version_1") / path.relative_to(lensfun)) for path in lensfun_paths]
    locale = prefix / "share/locale"
    asset_sources += [(path, Path("locale") / path.relative_to(locale)) for path in translation_files(locale)]
    report = {"abi": "arm64-v8a", "minimumApi": 26, "libraries": libraries,
              "configuredLibraries": configured_libraries,
              "systemLibraries": sorted(closure.system), "systemAllowlist": sorted(SYSTEM_LIBRARIES),
              "api26SystemStubsVerified": args.ndk is not None,
              "assets": {}, "status": "staged; Android loading and RAW/edit/export execution remain unverified"}
    output.parent.mkdir(parents=True, exist_ok=True)
    temporary = Path(tempfile.mkdtemp(prefix=f".{output.name}-", dir=output.parent))
    try:
        native = temporary / "jniLibs/arm64-v8a"
        native.mkdir(parents=True)
        for name, item in libraries.items():
            destination = native / name
            shutil.copyfile(item["source"], destination)
            destination.chmod(0o644)
            if sha256(destination) != item["sha256"]:
                raise StageError(f"library changed while copying: {item['source']}")
            copied = read_elf(destination)
            if (copied.soname != item["soname"] or list(copied.needed) != item["needed"]
                    or copied.search_paths != item["searchPaths"]):
                raise StageError(f"ELF metadata changed while copying: {item['source']}")
        for source, relative in asset_sources:
            destination = temporary / "assets/darktable-runtime" / relative
            destination.parent.mkdir(parents=True, exist_ok=True)
            digest = sha256(source)
            shutil.copyfile(source, destination)
            destination.chmod(0o644)
            if sha256(destination) != digest:
                raise StageError(f"runtime data changed while copying: {source}")
            report["assets"][relative.as_posix()] = {"source": str(source), "sha256": digest}
        data_files(temporary / "assets/darktable-runtime")
        (temporary / "android-runtime-libraries.txt").write_text(
            "\n".join(configured_libraries) + "\n", encoding="utf-8")
        (temporary / "manifest.json").write_text(json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8")
        if output.exists():
            raise StageError(f"staging destination appeared during copy: {output}")
        temporary.rename(output)
    finally:
        if temporary.exists():
            shutil.rmtree(temporary)
    print(f"Staged {len(libraries)} Android libraries and {len(asset_sources)} data files in {output}")
    print("Android loading, complete runtime coverage and RAW/edit/export execution still require verification.")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--prefix", type=Path, help="complete cmake --install prefix of the Android library-only engine")
    parser.add_argument("--output", type=Path, help="new, nonexisting APK source-set staging directory")
    parser.add_argument("--library-root", type=Path, action="append", default=[], help="additional Android shared-library directory (repeatable, searched recursively)")
    parser.add_argument("--ndk", type=Path, help="NDK root: verify API 26 system stubs and resolve libc++_shared/libomp")
    parser.add_argument("--lensfun-data", type=Path, help="installed Lensfun version_1 directory; default PREFIX/share/lensfun/version_1")
    parser.add_argument("--inspect-library", type=Path, action="append", default=[], help="inspect real ARM64 shared-library closure only; does not stage an engine")
    args = parser.parse_args()
    if args.inspect_library:
        if args.prefix or args.output or args.lensfun_data:
            parser.error("--inspect-library cannot be combined with staging options")
    elif not args.prefix or not args.output:
        parser.error("staging requires --prefix and --output")
    try:
        roots = list(args.library_root)
        if args.prefix:
            roots += [path for path in (args.prefix / "lib", args.prefix / "lib64") if path.is_dir()]
        if args.inspect_library:
            # Explicit seed directories also resolve dependencies shared by the
            # inspected probes; all non-system dependencies remain mandatory.
            roots += sorted({path.parent for path in args.inspect_library})
        closure = Closure(roots, args.ndk)
        if args.inspect_library:
            libraries = closure.collect(args.inspect_library)
            print(json.dumps({"mode": "library inspection only", "abi": "arm64-v8a", "minimumApi": 26,
                              "libraries": libraries, "systemLibraries": sorted(closure.system)}, indent=2, sort_keys=True))
        else:
            stage(args, closure)
        return 0
    except (StageError, OSError, UnicodeError) as error:
        print(f"Android runtime staging failed: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
