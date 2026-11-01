# SPDX-License-Identifier: MIT
"""Backport the pinned official database with Lensfun's own schema converter.

The downloaded source archive is SHA-512 verified by vcpkg. No calibration
coefficients or camera aliases are supplied by this wrapper.
"""
from __future__ import annotations

import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import shutil
import tempfile
import xml.etree.ElementTree as ET


class DatabaseError(ValueError):
    pass


def primary_name(element: ET.Element, tag: str) -> str:
    return next((child.text or "" for child in element.findall(tag)
                 if "lang" not in child.attrib), "")


def identities(root: ET.Element) -> list[tuple[str, str, str]]:
    return [(element.tag, primary_name(element, "maker"), primary_name(element, "model"))
            for element in root if element.tag in ("camera", "lens")]


def validate_database(directory: Path, required_cameras: list[dict]) -> dict:
    """Reject unsupported data and ensure the requested bodies have calibration."""
    files = sorted(directory.glob("*.xml"))
    if not files:
        raise DatabaseError("database contains no XML files")
    cameras, lenses = [], []
    for path in files:
        root = ET.parse(path).getroot()
        if root.tag != "lensdatabase" or root.get("version") != "1":
            raise DatabaseError(f"{path.name}: expected Lensfun schema version 1")
        for calibration in root.findall(".//calibration"):
            for correction in calibration:
                if correction.get("model") == "acm":
                    raise DatabaseError(f"{path.name}: ACM requires a newer Lensfun runtime")
                if correction.tag == "distortion" and "real-focal" in correction.attrib:
                    raise DatabaseError(f"{path.name}: schema 2 real-focal attribute remains")
        cameras.extend(root.findall("camera"))
        lenses.extend(root.findall("lens"))
    required_matches = {}
    for required in required_cameras:
        body = next((camera for camera in cameras
                     if primary_name(camera, "maker") == required["maker"]
                     and primary_name(camera, "model") == required["model"]), None)
        label = f'{required["maker"]} {required["model"]}'
        if body is None:
            raise DatabaseError(f"required camera is missing: {label}")
        mount = body.findtext("mount")
        matches = [primary_name(lens, "model") for lens in lenses
                   if mount and mount in [item.text for item in lens.findall("mount")]
                   and lens.find("calibration/distortion") is not None
                   # Lensfun 0.3.4 lens.cpp:Compare() allows 4% crop-factor rounding.
                   and float(body.findtext("cropfactor", "0")) >= float(lens.findtext("cropfactor", "0")) * 0.96]
        if not matches:
            raise DatabaseError(f"required camera has no compatible distortion calibration: {label}")
        required_matches[label] = matches
    return {"xml_files": len(files), "cameras": len(cameras), "lenses": len(lenses),
            "required_camera_lenses": required_matches}


def load_converter(source: Path, lock: dict):
    converter_path = source / lock["converter"]
    if hashlib.sha256(converter_path.read_bytes()).hexdigest() != lock["converter_sha256"]:
        raise DatabaseError("official converter SHA-256 differs from database-lock.json")
    spec = importlib.util.spec_from_file_location("lensfun_official_xml_converter", converter_path)
    if not spec or not spec.loader:
        raise DatabaseError("cannot import the official Lensfun converter")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def replace_database(staged: Path, output: Path, replace: bool):
    if output.exists():
        if not replace:
            raise DatabaseError("output already exists; use --replace to replace the complete database")
        shutil.rmtree(output)
    shutil.copytree(staged, output)


def convert_database(source: Path, output: Path, lock: dict, replace: bool = False) -> dict:
    source, output = source.resolve(), output.resolve()
    if source == output or source in output.parents or output in source.parents:
        raise DatabaseError("source and output directories must be separate")
    if lock["database_version"] != 1:
        raise DatabaseError("this overlay requires database version 1 for Lensfun 0.3.4")
    module = load_converter(source, lock)
    # Use the very same From2To1 class as upstream's version_1 update tarball.
    converter = module.From2To1()
    source_files = sorted((source / "data/db").glob("*.xml"))
    if not source_files:
        raise DatabaseError("official source contains no database XML files")
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="lensfun-schema1-", dir=output.parent) as temporary:
        staged = Path(temporary) / "database"
        staged.mkdir()
        for path in source_files:
            original = ET.parse(path).getroot()
            if original.tag != "lensdatabase" or original.get("version") != "2":
                raise DatabaseError(f"{path.name}: expected the pinned upstream schema version 2")
            xml_file = module.XMLFile(str(path))
            converter(xml_file.tree)
            module.XMLFile.indent(xml_file.tree.getroot())
            target = staged / path.name
            target.write_bytes(module.etree.tostring(xml_file.tree.getroot(), encoding="utf-8"))
            if identities(ET.parse(target).getroot()) != identities(original):
                raise DatabaseError(f"{path.name}: conversion changed camera/lens identities")
        report = validate_database(staged, lock["required_cameras"])
        (staged / "timestamp.txt").write_text(f'{lock["timestamp"]}\n', encoding="utf-8")
        provenance = dict(lock, validation=report)
        (staged / "database-provenance.json").write_text(
            json.dumps(provenance, indent=2, sort_keys=True) + "\n", encoding="utf-8")
        replace_database(staged, output, replace)
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--lock", type=Path, default=Path(__file__).with_name("database-lock.json"))
    parser.add_argument("--replace", action="store_true")
    args = parser.parse_args()
    try:
        report = convert_database(args.source, args.output,
                                  json.loads(args.lock.read_text(encoding="utf-8")), args.replace)
    except (DatabaseError, ET.ParseError, OSError, ImportError) as error:
        parser.exit(1, f"Lensfun database conversion failed: {error}\n")
    print(json.dumps(report, indent=2, sort_keys=True))


if __name__ == "__main__":
    main()
