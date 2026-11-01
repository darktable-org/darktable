# Lensfun database pin

The library remains Lensfun 0.3.4. Its original database predates GoPro HERO10
and HERO11. `database-lock.json` separately pins the official Lensfun database
to commit `bc5615d6278cb5fb75dd53e74015367453d5591d` (2026-10-28), including
the source archive SHA-512 and the upstream converter SHA-256.

The port downloads this immutable source and imports its unmodified
`tools/update_database/xml_converter.py` `From2To1` converter. This is the
converter used by the official `lensfun-update-data` schema-1 update bundles.
It removes ACM calibrations unsupported by 0.3.4 and moves real focal length
to its schema-1 element. Existing supported calibration coefficients and
camera/lens identities are retained. The whole old XML directory is replaced,
so profiles from different database revisions cannot be merged accidentally.

The host conversion uses an isolated vcpkg Python environment with
`lxml==6.0.2`; no Python package is added to the Android runtime. Every generated
XML is checked for schema version 1 and unsupported schema-2 calibration data.
The build also requires `GoPro / Hero10 black` and `GoPro / Hero11 Black` to
have a compatible lens with distortion calibration. The installed
`share/lensfun/version_1/database-provenance.json` records the pin and counts,
and is included in the Android runtime assets by the existing staging tool.
The overlay's port revision invalidates binary caches when this data changes.

This pin has lenses named `HERO10 Black & compatibles` and
`HERO11 Black & compatibles`. It has no HERO12 camera profile. The port adds
no aliases or invented calibrations; a compatible lens need not share the
camera's model name, and an unknown body must not be treated as supported.

The database is © its Lensfun contributors, licensed under CC BY-SA 3.0;
the original license at `data/COPYING.CC_BY-SA_3.0` is installed with the port's
copyright notices. Source and attribution are available at
<https://github.com/lensfun/lensfun/tree/bc5615d6278cb5fb75dd53e74015367453d5591d/data/db>.
Upstream's `README.md` LICENSE section places tools such as the converter
in the public domain unless their file header says otherwise; this converter
has no alternative license header. The wrapper in this overlay is MIT.

For an offline conversion after downloading and verifying the pinned archive:

```sh
python3 mobile/dependencies/overlays/lensfun/convert_database.py \
  --source /path/to/lensfun-bc5615d6278cb5fb75dd53e74015367453d5591d \
  --output /path/to/new-version-1-database
python3 -B -m unittest discover -s mobile/dependencies/tests -v
```
