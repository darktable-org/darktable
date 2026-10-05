"""Check CICP HDR import/export against independently encoded reference images.

Requires numpy and ImageMagick. Fixtures are the img/ directory from
https://github.com/kennylevinsen/hdr-test-images.
The output directory retains CLI logs, exports and a JSON result for diagnosis.
"""
from __future__ import annotations

import argparse
import json
import shutil
import subprocess
from pathlib import Path

import numpy as np

REFERENCE_WHITE_NITS = 203.0
PQ_PEAK_NITS = 10000.0
HLG_PEAK_NITS = 1000.0
HLG_GAMMA = 1.2
IMPORT_TOLERANCE = 0.003
EXPORT_TOLERANCE = 0.003

# Independent D65 gamut matrices used by the reference generator.
GAMUT = {
    "rec2020": np.array([[0.627504, 0.329275, 0.043303],
                         [0.069108, 0.919519, 0.011360],
                         [0.016394, 0.088011, 0.895380]]),
    "p3": np.array([[0.822462, 0.177538, 0.0],
                    [0.033194, 0.966806, 0.0],
                    [0.017083, 0.072397, 0.910520]]),
}
LUMA = {"rec2020": np.array([0.2627, 0.6780, 0.0593]),
        "p3": np.array([0.2290, 0.6917, 0.0793])}


def read_png(path: Path, magick: str) -> np.ndarray:
    dimensions = subprocess.check_output(
        [magick, "identify", "-format", "%w %h", str(path)], text=True
    ).split()
    width, height = map(int, dimensions)
    raw = subprocess.check_output(
        [magick, str(path), "-depth", "16", "-endian", "LSB", "rgb:-"]
    )
    return np.frombuffer(raw, dtype="<u2").reshape(height, width, 3) / 65535.0


def srgb_decode(signal: np.ndarray) -> np.ndarray:
    return np.where(signal <= 0.04045, signal / 12.92,
                    ((signal + 0.055) / 1.055) ** 2.4)


def hdr_decode(signal: np.ndarray, transfer: str, primaries: str) -> np.ndarray:
    if transfer == "pq":
        m1, m2 = 2610.0 / 16384.0, 2523.0 / 32.0
        c1, c2, c3 = 3424.0 / 4096.0, 2413.0 / 128.0, 2392.0 / 128.0
        p = signal ** (1.0 / m2)
        linear = np.maximum(p - c1, 0.0) / (c2 - c3 * p)
        linear = linear ** (1.0 / m1) * PQ_PEAK_NITS / REFERENCE_WHITE_NITS
    else:
        a, b, c = 0.17883277, 0.28466892, 0.55991073
        scene = np.where(signal <= 0.5, signal**2 / 3.0,
                         (np.exp((signal - c) / a) + b) / 12.0)
        luminance = np.sum(scene * LUMA[primaries], axis=-1, keepdims=True)
        linear = scene * luminance ** (HLG_GAMMA - 1.0)
        linear *= HLG_PEAK_NITS / REFERENCE_WHITE_NITS
    return linear @ np.linalg.inv(GAMUT[primaries]).T


def export(cli: Path, source: Path, output: Path, profile: str,
           force_lcms: bool) -> None:
    case = output.stem
    command = [str(cli), str(source), str(output), "--icc-type", profile,
               "--apply-custom-presets", "false", "--core",
               "--configdir", str(output.parent / (case + "-config")),
               "--cachedir", str(output.parent / (case + "-cache")),
               "--library", ":memory:", "--disable-opencl", "--threads", "4",
               "--conf", "plugins/darkroom/workflow=none",
               "--conf", "plugins/imageio/format/png/bpp=16",
               "--conf", "plugins/lighttable/export/force_lcms2=" + str(force_lcms).lower()]
    # Never reuse a previous image as evidence for a failed current invocation.
    output.unlink(missing_ok=True)
    with output.with_suffix(".log").open("w") as log:
        subprocess.run(command, stdout=log, stderr=log, check=True, timeout=120)
    if not output.is_file():
        raise AssertionError(f"CLI produced no output for {case}")


def fixture(root: Path, transfer: str, primaries: str, extension: str) -> Path:
    suffix = "_p3" if primaries == "p3" else ""
    path = root / f"test_{transfer}{suffix}.{extension}"
    if not path.exists() and not suffix:
        path = root / f"test-{transfer}.{extension}"
    if not path.is_file():
        raise FileNotFoundError(path)
    return path


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cli", type=Path, required=True)
    parser.add_argument("--fixtures", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--smoke", action="store_true", help="AVIF Rec2020 only")
    args = parser.parse_args()
    magick = shutil.which("magick")
    if magick is None:
        parser.error("ImageMagick is required for unmodified 16-bit PNG sample decoding")
    cli = args.cli.resolve()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    (output / "results.json").unlink(missing_ok=True)
    source = fixture(args.fixtures, "srgb", "rec2020", "avif")
    baseline = output / "srgb-reference.png"
    export(cli, source, baseline, "SRGB", False)
    reference = read_png(baseline, magick)
    cases: list[dict[str, str | float]] = []
    formats = ("avif",) if args.smoke else ("avif", "png", "heif", "jxl")
    primaries_list = ("rec2020",) if args.smoke else ("rec2020", "p3")
    for extension in formats:
        for primaries in primaries_list:
            for transfer in ("pq", "hlg"):
                name = f"import-{extension}-{primaries}-{transfer}"
                path = output / (name + ".png")
                export(cli, fixture(args.fixtures, transfer, primaries, extension),
                       path, "SRGB", False)
                error = float(np.max(np.abs(read_png(path, magick) - reference)))
                cases.append({"case": name, "max_error": error})
                if error > IMPORT_TOLERANCE:
                    raise AssertionError(f"{name}: max error {error}, limit {IMPORT_TOLERANCE}")
    for primaries in primaries_list:
        for transfer in ("pq", "hlg"):
            profile = transfer.upper() + "_" + primaries.upper()
            for force_lcms in (False, True):
                name = f"export-{primaries}-{transfer}-lcms-{force_lcms}"
                path = output / (name + ".png")
                export(cli, source, path, profile, force_lcms)
                linear = hdr_decode(read_png(path, magick), transfer, primaries)
                error = float(np.max(np.abs(linear - srgb_decode(reference))))
                cases.append({"case": name, "max_error": error})
                if error > EXPORT_TOLERANCE:
                    raise AssertionError(f"{name}: max error {error}, limit {EXPORT_TOLERANCE}")
                if not force_lcms:
                    # Our exported PNG also contains an ICC fallback. CICP
                    # must retain priority when that ICC has already been read.
                    imported = output / (name + "-icc-reimport.png")
                    export(cli, path, imported, "SRGB", False)
                    error = float(np.max(np.abs(read_png(imported, magick) - reference)))
                    cases.append({"case": name + "-icc-reimport", "max_error": error})
                    if error > IMPORT_TOLERANCE:
                        raise AssertionError(f"{name} ICC reimport: max error {error}")
    report = {"status": "passed", "cases": cases,
              "gpu_runtime": "not tested by this CPU regression harness"}
    (output / "results.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
