# SPDX-License-Identifier: GPL-3.0-or-later
"""Offline staging/APK regressions using synthetic ELF metadata, never engine code."""
from __future__ import annotations

import importlib.util
import hashlib
import json
from pathlib import Path
import stat
import struct
import sys
import tempfile
import unittest
from unittest.mock import patch
import warnings
import zipfile

sys.dont_write_bytecode = True
_path = Path(__file__).resolve().parents[1] / "verify-android-runtime-stage.py"
_spec = importlib.util.spec_from_file_location("android_runtime_verifier_test", _path)
assert _spec and _spec.loader
verifier = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(verifier)
stage = verifier.stage


def elf_bytes(soname: str, needed: tuple[str, ...] = (), search_path: str | None = None) -> bytes:
    """Only headers/dynamic strings required by read_elf; not executable code."""
    strings = bytearray(b"\0")
    entries = []
    for name, tag in [(soname, 14), *((name, 1) for name in needed)]:
        entries.append((tag, len(strings)))
        strings.extend(name.encode("ascii") + b"\0")
    if search_path is not None:
        entries.append((29, len(strings)))
        strings.extend(search_path.encode("ascii") + b"\0")
    dynamic_offset = 64 + 2 * 56
    string_offset = dynamic_offset + (len(entries) + 3) * 16
    entries.extend(((5, string_offset), (10, len(strings)), (0, 0)))
    size = string_offset + len(strings)
    header = b"\x7fELF\x02\x01\x01" + bytes(9)
    header += struct.pack("<HHIQQQIHHHHHH", 3, 183, 1, 0, 64, 0, 0, 64, 56, 2, 0, 0, 0)
    load = struct.pack("<IIQQQQQQ", 1, 4, 0, 0, 0, size, size, 4096)
    dynamic = struct.pack("<IIQQQQQQ", 2, 4, dynamic_offset, dynamic_offset, 0,
                          len(entries) * 16, len(entries) * 16, 8)
    return header + load + dynamic + b"".join(struct.pack("<qQ", *entry) for entry in entries) + strings


class RuntimeStageTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.directory = Path(self.temporary.name)
        self.root = self.directory / "stage"
        self.native = self.root / "jniLibs/arm64-v8a"
        self.runtime = self.root / "assets/darktable-runtime"
        self.native.mkdir(parents=True)
        self.configured = ["libdarktable.so", "libdt_mobile.so", "libdt_iop_exposure.so",
                           "libdt_format_png.so", "libdt_storage_disk.so"]
        self.modules = self.root / "android-runtime-libraries.txt"
        self.modules.write_text("\n".join(self.configured) + "\n")
        libraries = {}
        for name in self.configured:
            needed = ("libdarktable.so", "libc.so") if name == "libdt_mobile.so" else ("libc.so",)
            path = self.native / name
            path.write_bytes(elf_bytes(name, needed))
            elf = stage.read_elf(path)
            libraries[name] = {"sha256": stage.sha256(path), "soname": elf.soname,
                               "needed": list(elf.needed), "searchPaths": elf.search_paths}
        assets = {}
        for name in [*(f"data/{name}" for name in stage.REQUIRED_DATA), "lensfun/version_1/test.xml",
                     "locale/test/LC_MESSAGES/darktable.mo"]:
            path = self.runtime / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text("test fixture\n")
            assets[name] = {"sha256": stage.sha256(path)}
        # The fixture flag exercises verification of producer reports. These
        # synthetic ELF files are not NDK output and make no Android claim.
        self.manifest = {"abi": "arm64-v8a", "minimumApi": 26,
                         "api26SystemStubsVerified": True,
                         "systemAllowlist": sorted(stage.SYSTEM_LIBRARIES),
                         "systemLibraries": ["libc.so"],
                         "configuredLibraries": self.configured,
                         "libraries": libraries, "assets": assets}
        self.write_manifest()

    def write_manifest(self):
        (self.root / "manifest.json").write_text(json.dumps(self.manifest))

    def apk(self, name="test.apk", transform=lambda name, data: data, extra=()):
        path = self.directory / name
        with zipfile.ZipFile(path, "w") as archive:
            archive.writestr("AndroidManifest.xml", b"synthetic APK manifest")
            for source in [*self.native.iterdir(), *stage.data_files(self.runtime)]:
                name = (f"lib/arm64-v8a/{source.name}" if source.parent == self.native
                        else f"assets/darktable-runtime/{source.relative_to(self.runtime).as_posix()}")
                data = transform(name, source.read_bytes())
                if data is not None:
                    archive.writestr(name, data)
            for name, data in extra:
                archive.writestr(name, data)
        return path

    def test_complete_stage_generated_copy_and_final_apk(self):
        output = self.directory / "generated"
        report = verifier.copy_verified(self.root, self.modules, output)
        self.assertEqual(report["libraries"], 5)
        self.assertEqual(report["assets"], 6)
        self.assertEqual(verifier.copy_verified(self.root, self.modules, output), report)
        apk = self.apk()
        report = verifier.verify_apk(self.root, self.modules, apk)
        self.assertEqual(report["apkSha256"], stage.sha256(apk))

    def test_apk_changed_native_bytes_after_generated_sources_fail(self):
        apk = self.apk(transform=lambda name, data: data + b"stripped" if name.endswith("libdarktable.so") else data)
        with self.assertRaisesRegex(stage.StageError, "native library hash changed"):
            verifier.verify_apk(self.root, self.modules, apk)

    def test_apk_missing_dynamic_plugin_fails(self):
        apk = self.apk(transform=lambda name, data: None if "libdt_iop_" in name else data)
        with self.assertRaisesRegex(stage.StageError, "native files do not match"):
            verifier.verify_apk(self.root, self.modules, apk)

    def test_apk_missing_runtime_asset_fails(self):
        apk = self.apk(transform=lambda name, data: None if name.endswith("cameras.xml") else data)
        with self.assertRaisesRegex(stage.StageError, "runtime assets do not match"):
            verifier.verify_apk(self.root, self.modules, apk)

    def test_stage_missing_translations_fails_even_with_matching_manifest(self):
        name = "locale/test/LC_MESSAGES/darktable.mo"
        (self.runtime / name).unlink()
        del self.manifest["assets"][name]
        self.write_manifest()
        with self.assertRaisesRegex(stage.StageError, "no nonempty LC_MESSAGES/darktable.mo"):
            verifier.verify(self.root, self.modules)

    def test_apk_missing_translation_fails(self):
        apk = self.apk(transform=lambda name, data: None if name.endswith("darktable.mo") else data)
        with self.assertRaisesRegex(stage.StageError, "runtime assets do not match"):
            verifier.verify_apk(self.root, self.modules, apk)

    def test_translation_must_have_the_language_directory_layout(self):
        original = "locale/test/LC_MESSAGES/darktable.mo"
        incorrect = "locale/LC_MESSAGES/darktable.mo"
        path = self.runtime / incorrect
        path.parent.mkdir(parents=True)
        (self.runtime / original).rename(path)
        self.manifest["assets"][incorrect] = self.manifest["assets"].pop(original)
        self.write_manifest()
        with self.assertRaisesRegex(stage.StageError, "no nonempty LC_MESSAGES/darktable.mo"):
            verifier.verify(self.root, self.modules)

    def test_apk_extra_abi_fails(self):
        apk = self.apk(extra=[("lib/x86_64/libdarktable.so", b"wrong ABI")])
        with self.assertRaisesRegex(stage.StageError, "only the arm64-v8a ABI"):
            verifier.verify_apk(self.root, self.modules, apk)

    def app_native(self, data=None):
        name = "libandroidx.graphics.path.so"
        if data is None:
            data = elf_bytes(name, ("libc.so", "libdarktable.so"))
        self.app_pin = patch.dict(verifier.APP_NATIVE_LIBRARIES,
                                  {name: hashlib.sha256(data).hexdigest()}, clear=True)
        self.app_pin.start()
        self.addCleanup(self.app_pin.stop)
        return (f"lib/arm64-v8a/{name}", data)

    def test_pinned_application_native_library_is_verified_separately(self):
        apk = self.apk(extra=[self.app_native()])
        report = verifier.verify_apk(self.root, self.modules, apk)
        self.assertEqual(report["libraries"], 5)
        self.assertEqual(report["appNativeLibraries"], 1)

    def test_application_native_library_changed_bytes_fail(self):
        name, data = self.app_native()
        apk = self.apk(extra=[(name, data + b"changed")])
        with self.assertRaisesRegex(stage.StageError, "application native library hash changed"):
            verifier.verify_apk(self.root, self.modules, apk)

    def test_application_native_library_unresolved_dependency_fails(self):
        entry = self.app_native(elf_bytes("libandroidx.graphics.path.so", ("libmissing.so",)))
        apk = self.apk(extra=[entry])
        with self.assertRaisesRegex(stage.StageError, "unresolved application native dependency"):
            verifier.verify_apk(self.root, self.modules, apk)

    def test_application_native_library_wrong_machine_fails(self):
        data = bytearray(elf_bytes("libandroidx.graphics.path.so"))
        struct.pack_into("<H", data, 18, 62)  # x86_64, despite its ARM64 ZIP path.
        apk = self.apk(extra=[self.app_native(bytes(data))])
        with self.assertRaisesRegex(stage.StageError, "expected an AArch64 ET_DYN library"):
            verifier.verify_apk(self.root, self.modules, apk)

    def test_application_native_library_wrong_abi_directory_fails(self):
        name, data = self.app_native()
        apk = self.apk(extra=[(name.replace("arm64-v8a", "x86_64"), data)])
        with self.assertRaisesRegex(stage.StageError, "only the arm64-v8a ABI"):
            verifier.verify_apk(self.root, self.modules, apk)

    def test_application_native_library_wrong_soname_fails(self):
        apk = self.apk(extra=[self.app_native(elf_bytes("libwrong.so"))])
        with self.assertRaisesRegex(stage.StageError, "application native SONAME"):
            verifier.verify_apk(self.root, self.modules, apk)

    def test_application_native_library_unsafe_search_path_fails(self):
        entry = self.app_native(elf_bytes("libandroidx.graphics.path.so", search_path="/host/libs"))
        apk = self.apk(extra=[entry])
        with self.assertRaisesRegex(stage.StageError, "incompatible with a flat nativeLibraryDir"):
            verifier.verify_apk(self.root, self.modules, apk)

    def test_unknown_application_native_library_fails(self):
        apk = self.apk(extra=[("lib/arm64-v8a/libunknown.so", elf_bytes("libunknown.so"))])
        with self.assertRaisesRegex(stage.StageError, "native files do not match"):
            verifier.verify_apk(self.root, self.modules, apk)

    def test_application_native_allowlist_does_not_relax_stage_verification(self):
        name, data = self.app_native()
        (self.native / Path(name).name).write_bytes(data)
        with self.assertRaisesRegex(stage.StageError, "native files do not match"):
            verifier.verify(self.root, self.modules)

    def test_apk_duplicate_native_entry_fails(self):
        with warnings.catch_warnings():
            warnings.simplefilter("ignore", UserWarning)
            apk = self.apk(extra=[("lib/arm64-v8a/libdt_mobile.so", b"duplicate")])
        with self.assertRaisesRegex(stage.StageError, "duplicate APK entry"):
            verifier.verify_apk(self.root, self.modules, apk)

    def test_apk_ambiguous_path_fails_without_writing_outside_temporary_tree(self):
        apk = self.apk(extra=[("assets/../../outside", b"invalid path")])
        with self.assertRaisesRegex(stage.StageError, "invalid runtime asset name"):
            verifier.verify_apk(self.root, self.modules, apk)
        self.assertFalse((self.directory / "outside").exists())

    def test_apk_symlink_fails(self):
        apk = self.apk()
        with zipfile.ZipFile(apk, "a") as archive:
            entry = zipfile.ZipInfo("assets/darktable-runtime/symlink")
            entry.create_system = 3
            entry.external_attr = (stat.S_IFLNK | 0o777) << 16
            archive.writestr(entry, "data/darktablerc")
        with self.assertRaisesRegex(stage.StageError, "must not be a symlink"):
            verifier.verify_apk(self.root, self.modules, apk)

    def test_unmanaged_output_is_retained(self):
        output = self.directory / "unmanaged"
        output.mkdir()
        (output / "keep").write_text("user content")
        with self.assertRaisesRegex(stage.StageError, "unmanaged output"):
            verifier.copy_verified(self.root, self.modules, output)
        self.assertEqual((output / "keep").read_text(), "user content")

    def test_unresolved_shared_dependency_fails(self):
        path = self.native / "libdarktable.so"
        path.write_bytes(elf_bytes(path.name, ("libmissing.so",)))
        self.manifest["libraries"][path.name].update(sha256=stage.sha256(path), needed=["libmissing.so"])
        self.write_manifest()
        with self.assertRaisesRegex(stage.StageError, "unresolved native dependency"):
            verifier.verify(self.root, self.modules)

    def test_stage_cannot_substitute_dependency_only_jni(self):
        path = self.native / "libdt_mobile.so"
        path.write_bytes(elf_bytes(path.name, ("libc.so",)))
        self.manifest["libraries"][path.name].update(sha256=stage.sha256(path), needed=["libc.so"])
        self.write_manifest()
        with self.assertRaisesRegex(stage.StageError, "not linked to the processing core"):
            verifier.verify(self.root, self.modules)


if __name__ == "__main__":
    unittest.main()
