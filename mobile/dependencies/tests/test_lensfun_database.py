# SPDX-License-Identifier: GPL-3.0-or-later
"""Offline validation of schema compatibility and complete database replacement."""
from __future__ import annotations

import importlib.util
import json
from pathlib import Path
import sys
import tempfile
import unittest

sys.dont_write_bytecode = True
_overlay = Path(__file__).resolve().parents[1] / "overlays/lensfun"
_spec = importlib.util.spec_from_file_location("lensfun_database_test", _overlay / "convert_database.py")
assert _spec and _spec.loader
database = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(database)

# Synthetic compatibility fixture; not a calibration shipped in the product.
XML = """<lensdatabase version="1">
<camera><maker>Fixture</maker><model>Body A</model><mount>fixed-a</mount><cropfactor>5.5</cropfactor></camera>
<lens><maker>Fixture</maker><model>Different lens name</model><model lang="en">fixed lens</model>
<mount>fixed-a</mount><cropfactor>5.5</cropfactor><calibration>
<distortion model="ptlens" focal="2.7" a="0" b="0" c="0"/>
</calibration></lens></lensdatabase>"""
REQUIRED = [{"maker": "Fixture", "model": "Body A"}]


class LensfunDatabaseTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.staged = self.root / "converted"
        self.staged.mkdir()
        self.xml = self.staged / "actioncams.xml"
        self.xml.write_text(XML, encoding="utf-8")

    def test_compatible_mount_and_calibration_accept_different_lens_name(self):
        report = database.validate_database(self.staged, REQUIRED)
        self.assertEqual(report, {"xml_files": 1, "cameras": 1, "lenses": 1,
                                 "required_camera_lenses": {"Fixture Body A": ["Different lens name"]}})

    def test_schema_two_is_rejected_before_packaging(self):
        self.xml.write_text(XML.replace('version="1"', 'version="2"'), encoding="utf-8")
        with self.assertRaisesRegex(database.DatabaseError, "schema version 1"):
            database.validate_database(self.staged, REQUIRED)

    def test_acm_cannot_be_relabelled_as_schema_one(self):
        self.xml.write_text(XML.replace('model="ptlens"', 'model="acm"'), encoding="utf-8")
        with self.assertRaisesRegex(database.DatabaseError, "ACM requires a newer"):
            database.validate_database(self.staged, REQUIRED)

    def test_unconverted_real_focal_attribute_is_rejected(self):
        self.xml.write_text(XML.replace('focal="2.7"', 'focal="2.7" real-focal="2.5"'), encoding="utf-8")
        with self.assertRaisesRegex(database.DatabaseError, "schema 2 real-focal"):
            database.validate_database(self.staged, REQUIRED)

    def test_required_camera_cannot_silently_disappear(self):
        with self.assertRaisesRegex(database.DatabaseError, "required camera is missing"):
            database.validate_database(self.staged, [{"maker": "Fixture", "model": "Body B"}])

    def test_camera_without_compatible_lens_is_rejected(self):
        self.xml.write_text(XML.replace('<mount>fixed-a</mount><cropfactor>5.5</cropfactor><calibration>',
                                        '<mount>different</mount><cropfactor>5.5</cropfactor><calibration>'),
                            encoding="utf-8")
        with self.assertRaisesRegex(database.DatabaseError, "no compatible distortion calibration"):
            database.validate_database(self.staged, REQUIRED)

    def test_lens_without_distortion_does_not_satisfy_required_body(self):
        self.xml.write_text(XML.replace('<distortion ', '<tca '), encoding="utf-8")
        with self.assertRaisesRegex(database.DatabaseError, "no compatible distortion calibration"):
            database.validate_database(self.staged, REQUIRED)

    def test_lens_crop_factor_must_cover_body(self):
        self.xml.write_text(XML.replace('<cropfactor>5.5</cropfactor><calibration>',
                                        '<cropfactor>4.5</cropfactor><calibration>'), encoding="utf-8")
        # This ratio is compatible. Reversing it must not pass the build guard.
        database.validate_database(self.staged, REQUIRED)
        self.xml.write_text(XML.replace('<cropfactor>5.5</cropfactor><calibration>',
                                        '<cropfactor>6.5</cropfactor><calibration>'), encoding="utf-8")
        with self.assertRaisesRegex(database.DatabaseError, "no compatible distortion calibration"):
            database.validate_database(self.staged, REQUIRED)

    def test_complete_replacement_removes_stale_profiles(self):
        output = self.root / "installed"
        output.mkdir()
        (output / "obsolete-camera.xml").write_text(XML, encoding="utf-8")
        database.replace_database(self.staged, output, replace=True)
        self.assertEqual([p.name for p in output.iterdir()], ["actioncams.xml"])
        self.assertEqual(database.validate_database(output, REQUIRED)["cameras"], 1)

    def test_crop_factor_rounding_matches_lensfun_034_tolerance(self):
        self.xml.write_text(XML.replace('<cropfactor>5.5</cropfactor><calibration>',
                                        '<cropfactor>5.6</cropfactor><calibration>'), encoding="utf-8")
        database.validate_database(self.staged, REQUIRED)
        self.xml.write_text(XML.replace('<cropfactor>5.5</cropfactor><calibration>',
                                        '<cropfactor>5.8</cropfactor><calibration>'), encoding="utf-8")
        with self.assertRaisesRegex(database.DatabaseError, "no compatible distortion calibration"):
            database.validate_database(self.staged, REQUIRED)

    def test_replacement_requires_explicit_flag_and_preserves_existing_data(self):
        output = self.root / "installed"
        output.mkdir()
        marker = output / "keep.xml"
        marker.write_text("original", encoding="utf-8")
        with self.assertRaisesRegex(database.DatabaseError, "use --replace"):
            database.replace_database(self.staged, output, replace=False)
        self.assertEqual(marker.read_text(encoding="utf-8"), "original")

    def test_converter_hash_is_verified_before_loading_python(self):
        converter = self.root / "converter.py"
        converter.write_text('raise RuntimeError("must not execute")', encoding="utf-8")
        with self.assertRaisesRegex(database.DatabaseError, "SHA-256 differs"):
            database.load_converter(self.root, {"converter": converter.name, "converter_sha256": "0" * 64})

    def test_source_directory_cannot_be_replaced(self):
        with self.assertRaisesRegex(database.DatabaseError, "must be separate"):
            database.convert_database(self.staged, self.staged, {}, replace=True)

    def test_lock_pins_official_supported_bodies_without_hero12_alias(self):
        lock = json.loads((_overlay / "database-lock.json").read_text(encoding="utf-8"))
        self.assertEqual(lock["database_version"], 1)
        self.assertEqual(lock["required_cameras"], [
            {"maker": "GoPro", "model": "Hero10 black"},
            {"maker": "GoPro", "model": "Hero11 Black"}])
        self.assertEqual(len(lock["revision"]), 40)
        self.assertEqual(len(lock["archive_sha512"]), 128)


if __name__ == "__main__":
    unittest.main()
