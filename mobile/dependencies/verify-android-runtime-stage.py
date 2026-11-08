#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Verify and optionally copy a complete staged Android engine for Gradle."""
from __future__ import annotations

import argparse
import importlib.util
import json
from pathlib import Path
import shutil
import stat
import sys
import tempfile
import zipfile

# Import the same ELF/data checks as the staging producer without writing into
# the source tree. No target code is loaded or executed by this verifier.
sys.dont_write_bytecode = True
_parser_path = Path(__file__).with_name("stage-android-runtime.py")
_spec = importlib.util.spec_from_file_location("darktable_android_stage", _parser_path)
assert _spec and _spec.loader
stage = importlib.util.module_from_spec(_spec)
sys.modules[_spec.name] = stage
_spec.loader.exec_module(stage)

MANAGED_MARKER = "verified-prebuilt-stage.json"

# Compose uses the native path implementation from the strictly pinned
# androidx.graphics:graphics-path:1.0.1 AAR. It is an application dependency,
# not part of the independently staged desktop engine. Only its original ARM64
# bytes may accompany the engine; unknown or changed native files still fail.
# Official AAR SHA-256:
# 8ca4032b6d79b351f0b59ad4b580eddbb9423e1652f7c958830687f1eee2ec03
APP_NATIVE_LIBRARIES = {
    "libandroidx.graphics.path.so": "41e9a793c43a0f4fddb19e33f346bace464f30f888ba7b9eaf96294ea115bfb6",
}


def require(condition: bool, message: str) -> None:
    if not condition:
        raise stage.StageError(message)


def object_pairs(pairs):
    result = {}
    for key, value in pairs:
        require(key not in result, f"duplicate JSON key: {key}")
        result[key] = value
    return result


def relative_asset(name: str) -> Path:
    require(isinstance(name, str) and name and "\\" not in name,
            f"invalid runtime asset name: {name!r}")
    parts = name.split("/")
    require(all(part and part not in (".", "..") for part in parts),
            f"invalid runtime asset name: {name!r}")
    return Path(*parts)


def read_manifest(path: Path) -> dict:
    require(path.is_file() and not path.is_symlink(), f"stage manifest is missing or not a regular file: {path}")
    manifest = json.loads(path.read_text(encoding="utf-8"), object_pairs_hook=object_pairs)
    require(isinstance(manifest, dict), "stage manifest must be an object")
    require(manifest.get("abi") == "arm64-v8a" and manifest.get("minimumApi") == 26,
            "stage must target arm64-v8a / Android API 26")
    require(manifest.get("api26SystemStubsVerified") is True,
            "recreate the engine stage with --ndk to verify API 26 system stubs")
    require(set(manifest.get("systemAllowlist", [])) == stage.SYSTEM_LIBRARIES,
            "stage system-library allowlist differs from the Android API 26 policy")
    return manifest


def verify(root: Path, modules_path: Path, assets_root: Path | None = None,
           jni_root: Path | None = None) -> dict:
    require(root.is_dir() and not root.is_symlink(), f"Android runtime stage is missing or not a regular directory: {root}")
    root = root.resolve(strict=True)
    allowed = {"manifest.json", "android-runtime-libraries.txt", "assets", "jniLibs", MANAGED_MARKER}
    require(not (set(path.name for path in root.iterdir()) - allowed),
            "stage contains unexpected top-level files; use the staging producer's output")
    manifest = read_manifest(root / "manifest.json")
    require(modules_path.is_file() and not modules_path.is_symlink(),
            f"configured engine module manifest is missing: {modules_path}")
    configured = modules_path.read_text(encoding="utf-8").splitlines()
    require(configured and len(configured) == len(set(configured)), "configured engine module manifest is empty or has duplicate names")
    for name in configured:
        stage.library_name(name)
    require(manifest.get("configuredLibraries") == configured,
            "configuredLibraries does not match the original configured module manifest; recreate the stage")
    internal_modules = root / "android-runtime-libraries.txt"
    if internal_modules.exists() or internal_modules.is_symlink():
        require(internal_modules.is_file() and not internal_modules.is_symlink()
                and internal_modules.read_text(encoding="utf-8").splitlines() == configured,
                "the stage's module manifest differs from the configured module list")
    require({"libdarktable.so", "libdt_mobile.so"}.issubset(configured),
            "configured module manifest must include the processing core and JNI engine")
    for category in ("iop", "format", "storage"):
        require(any(name.startswith(f"libdt_{category}_") for name in configured),
                f"configured engine module manifest has no {category} plugins")

    jni = jni_root if jni_root is not None else root / "jniLibs"
    require(jni.is_dir() and not jni.is_symlink(), "stage jniLibs directory is missing")
    require({path.name for path in jni.iterdir()} == {"arm64-v8a"}, "only the arm64-v8a ABI may be packaged from this engine stage")
    native = jni / "arm64-v8a"
    require(native.is_dir() and not native.is_symlink(), "stage ARM64 native directory is missing")
    libraries = manifest.get("libraries")
    require(isinstance(libraries, dict) and libraries, "stage manifest has no native libraries")
    require({path.name for path in native.iterdir()} == set(libraries),
            "staged native files do not match the manifest exactly")
    require(set(configured).issubset(libraries), "stage omits configured engine modules")
    plugin_names = {name for name in libraries if name.startswith(("libdt_iop_", "libdt_format_", "libdt_storage_"))}
    require(plugin_names == (plugin_names & set(configured)), "stage includes plugins absent from the configured module manifest")
    actual_system = set()
    for name, metadata in libraries.items():
        stage.library_name(name)
        require(name not in stage.SYSTEM_LIBRARIES, f"Android system library must not be bundled: {name}")
        path = native / name
        require(path.is_file() and not path.is_symlink() and isinstance(metadata, dict), f"native library is not a regular file: {name}")
        require(metadata.get("sha256") == stage.sha256(path), f"native library hash changed: {name}")
        elf = stage.read_elf(path)
        require(elf.soname is None or elf.soname == name, f"native SONAME does not match filename: {name}")
        require(metadata.get("soname") == elf.soname and metadata.get("needed") == list(elf.needed)
                and metadata.get("searchPaths") == elf.search_paths, f"native ELF metadata changed: {name}")
        for needed in elf.needed:
            require(needed in libraries or needed in stage.SYSTEM_LIBRARIES,
                    f"unresolved native dependency: {name} needs {needed}")
            if needed in stage.SYSTEM_LIBRARIES:
                actual_system.add(needed)
        if name == "libdt_mobile.so":
            require("libdarktable.so" in elf.needed,
                    "JNI library is not linked to the processing core; a dependency-only probe cannot be packaged as an engine")
    require(set(manifest.get("systemLibraries", [])) == actual_system, "system-library dependency report differs from the actual ELF closure")

    assets = assets_root if assets_root is not None else root / "assets"
    require(assets.is_dir() and not assets.is_symlink(), "stage assets directory is missing")
    require({path.name for path in assets.iterdir()} == {"darktable-runtime"}, "stage assets must contain the complete darktable-runtime tree")
    runtime = assets / "darktable-runtime"
    require(not runtime.is_symlink(), "runtime asset directory must not be a symlink")
    paths = stage.data_files(runtime)
    recorded = manifest.get("assets")
    require(isinstance(recorded, dict) and recorded, "stage manifest has no runtime assets")
    require({path.relative_to(runtime).as_posix() for path in paths} == set(recorded),
            "runtime assets do not match the manifest exactly")
    for name, metadata in recorded.items():
        path = runtime / relative_asset(name)
        require(isinstance(metadata, dict) and metadata.get("sha256") == stage.sha256(path),
                f"runtime asset hash changed: {name}")
    for name in stage.REQUIRED_DATA:
        path = runtime / "data" / name
        require(path.is_file() and path.stat().st_size > 0, f"required runtime data is missing or empty: {name}")
    require(any(path.suffix == ".xml" and path.stat().st_size > 0
                for path in paths if path.is_relative_to(runtime / "lensfun/version_1")),
            "stage has no nonempty Lensfun version_1 calibration XML")
    stage.translation_files(runtime / "locale")
    return {"libraries": len(libraries), "configuredLibraries": len(configured), "assets": len(paths),
            "moduleManifestSha256": stage.sha256(modules_path)}


def checked_generated_output(output: Path) -> bool:
    """Return whether an existing nonempty directory belongs to this producer."""
    require(not output.is_symlink(), "generated runtime output must not be a symlink")
    if not output.exists():
        return False
    require(output.is_dir(), "generated runtime output must be a directory")
    # Gradle creates declared output directories before invoking the task action.
    if not any(output.iterdir()):
        return False
    marker = output / MANAGED_MARKER
    require(marker.is_file() and not marker.is_symlink(), "refusing to replace an unmanaged output directory")
    contents = json.loads(marker.read_text())
    require(isinstance(contents, dict) and contents.get("producer") == "darktable-prebuilt-gradle-v1",
            "generated output marker is invalid")
    return True


def copy_verified(root: Path, modules: Path, output: Path) -> dict:
    report = verify(root, modules)
    root, modules = root.resolve(strict=True), modules.resolve(strict=True)
    output = output.absolute()
    checked_generated_output(output)
    require(not output.resolve().is_relative_to(root) and not root.is_relative_to(output.resolve()),
            "generated runtime output and source stage must not contain one another")
    require(not modules.is_relative_to(output.resolve()), "module manifest must be outside the generated output")
    output.parent.mkdir(parents=True, exist_ok=True)
    temporary = Path(tempfile.mkdtemp(prefix=f".{output.name}-", dir=output.parent))
    try:
        shutil.copytree(root / "assets", temporary / "assets")
        shutil.copytree(root / "jniLibs", temporary / "jniLibs")
        shutil.copyfile(root / "manifest.json", temporary / "manifest.json")
        shutil.copyfile(modules, temporary / "android-runtime-libraries.txt")
        report = verify(temporary, temporary / "android-runtime-libraries.txt")
        (temporary / MANAGED_MARKER).write_text(json.dumps({"producer": "darktable-prebuilt-gradle-v1", **report}, indent=2) + "\n")
        if output.exists():
            if checked_generated_output(output):
                shutil.rmtree(output)
            else:
                output.rmdir()
        temporary.rename(output)
    finally:
        if temporary.exists():
            shutil.rmtree(temporary)
    return report


def verify_apk(root: Path, modules: Path, apk: Path) -> dict:
    """Check the final archive after AGP merging/stripping/signing.

    This is a byte-for-byte runtime audit, not APK signature, installation or
    Android execution validation. Extract only assets and native libraries into
    a private temporary directory; never load code from the archive.
    """
    verify(root, modules)
    engine_libraries = set(read_manifest(root / "manifest.json")["libraries"])
    require(apk.is_file() and not apk.is_symlink(), f"APK is missing or not a regular file: {apk}")
    with zipfile.ZipFile(apk) as archive, tempfile.TemporaryDirectory(prefix="darktable-apk-audit-") as directory:
        target = Path(directory)
        names = set()
        app_libraries = []
        for item in archive.infolist():
            name = item.filename
            require(name not in names, f"duplicate APK entry: {name}")
            names.add(name)
            # Reject ambiguous archive paths before selecting the runtime tree.
            # Android's ZIP reader must see the same names as this verifier.
            relative_asset(name[:-1] if item.is_dir() else name)
            if not name.startswith(("assets/", "lib/")):
                continue
            mode = item.external_attr >> 16
            require(not stat.S_ISLNK(mode), f"APK runtime entry must not be a symlink: {name}")
            relative = Path(name)
            destination = target / ("jniLibs" if relative.parts[0] == "lib" else "assets")
            destination = destination.joinpath(*relative.parts[1:])
            if item.is_dir():
                destination.mkdir(parents=True, exist_ok=True)
                continue
            destination.parent.mkdir(parents=True, exist_ok=True)
            with archive.open(item) as source, destination.open("xb") as output:
                shutil.copyfileobj(source, output, length=1024 * 1024)
            if name.startswith("lib/arm64-v8a/") and len(relative.parts) == 3:
                library = relative.name
                if library in APP_NATIVE_LIBRARIES:
                    require(library not in engine_libraries,
                            f"application native library collides with the engine: {library}")
                    require(stage.sha256(destination) == APP_NATIVE_LIBRARIES[library],
                            f"application native library hash changed: {library}")
                    elf = stage.read_elf(destination)
                    require(elf.soname is None or elf.soname == library,
                            f"application native SONAME does not match filename: {library}")
                    for needed in elf.needed:
                        require(needed in engine_libraries or needed in stage.SYSTEM_LIBRARIES,
                                f"unresolved application native dependency: {library} needs {needed}")
                    app_directory = target / "app-native"
                    app_directory.mkdir(exist_ok=True)
                    destination.rename(app_directory / library)
                    app_libraries.append(library)
        require("AndroidManifest.xml" in names, "archive has no AndroidManifest.xml")
        report = verify(root, modules, target / "assets", target / "jniLibs")
    return {**report, "appNativeLibraries": len(app_libraries), "apkSha256": stage.sha256(apk)}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--stage", required=True, type=Path)
    parser.add_argument("--modules", type=Path, help="original configured module manifest; defaults to STAGE/android-runtime-libraries.txt")
    parser.add_argument("--output", type=Path, help="copy verified sources into this Gradle-managed generated directory")
    parser.add_argument("--packaged-assets", type=Path, help="verify AGP's actual assets output against the stage manifest")
    parser.add_argument("--packaged-jni", type=Path, help="verify AGP's actual JNI output against the stage manifest")
    parser.add_argument("--apk", type=Path, help="verify the final APK's exact runtime bytes after packaging/signing")
    args = parser.parse_args()
    if (args.packaged_assets is None) != (args.packaged_jni is None):
        parser.error("--packaged-assets and --packaged-jni must be supplied together")
    if args.output and args.packaged_assets is not None:
        parser.error("--output cannot be combined with packaged-output verification")
    if args.apk and (args.output or args.packaged_assets is not None):
        parser.error("--apk cannot be combined with copying or packaged-output verification")
    modules = args.modules or args.stage / "android-runtime-libraries.txt"
    try:
        if args.apk:
            report = verify_apk(args.stage, modules, args.apk)
        elif args.output:
            report = copy_verified(args.stage, modules, args.output)
        else:
            report = verify(args.stage, modules, args.packaged_assets, args.packaged_jni)
        print(json.dumps({"status": "verified APK runtime files; Android execution remains unverified" if args.apk
                          else "verified staged files; Android execution remains unverified", **report}, sort_keys=True))
        return 0
    except (stage.StageError, OSError, ValueError, TypeError, KeyError, zipfile.BadZipFile) as error:
        print(f"Prebuilt Android runtime verification failed: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
