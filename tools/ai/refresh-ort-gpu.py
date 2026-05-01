#!/usr/bin/env python3
# This file is part of darktable, copyright (C) 2026 darktable developers.
# License GPL v3+.
"""
refresh-ort-gpu.py — keep data/ort_gpu.json in sync with upstream
ONNX Runtime release locations.

Scrapes:
  * GitHub releases of microsoft/onnxruntime (NVIDIA CUDA tarballs/zips)
  * https://repo.radeon.com/rocm/manylinux/      (AMD ROCm wheels)
  * PyPI onnxruntime-openvino + openvino           (Intel OpenVINO wheels)

For each (vendor, platform, accelerator-version) tuple in the existing
registry, looks up the freshest upstream URL and computes its SHA256.
If anything changed, prints a unified diff (--check), updates the file
in place (--update), or opens a PR (--pr).

We keep one entry per (CUDA major | ROCm minor) — wheel ABI is stable
within those granularities so finer pinning is unnecessary churn.
See data/ort_gpu.json comment / RAWDENOISE.md for rationale.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import logging
import os
import re
import subprocess
import sys
import tempfile
import urllib.request
import urllib.error
from collections.abc import Iterable
from html.parser import HTMLParser
from pathlib import Path

REGISTRY_PATH = Path(__file__).resolve().parents[2] / "data" / "ort_gpu.json"

RADEON_INDEX = "https://repo.radeon.com/rocm/manylinux/"
GH_RELEASES = "https://api.github.com/repos/microsoft/onnxruntime/releases"
PYPI_OPENVINO_ORT = "https://pypi.org/pypi/onnxruntime-openvino/json"
PYPI_OPENVINO_RUNTIME = "https://pypi.org/pypi/openvino/json"

# Python ABI tag we ship against. ONNX Runtime publishes cp310, cp311, cp312, cp313
# wheels — we standardize on cp312 to match the install scripts.
PY_TAG = "cp312"

# CUDA platforms / asset shape. The pattern is keyed by (platform, cuda_major).
CUDA_PATTERNS = {
    ("linux", 12): "onnxruntime-linux-x64-gpu-{ver}.tgz",
    ("linux", 13): "onnxruntime-linux-x64-gpu_cuda13-{ver}.tgz",
    ("windows", 12): "onnxruntime-win-x64-gpu-{ver}.zip",
    ("windows", 13): "onnxruntime-win-x64-gpu_cuda13-{ver}.zip",
}

# Intel: PyPI wheel platform tags per OS. cp312 cp312 is added by _find_pypi_wheel
INTEL_WHEEL_TAGS = {
    "linux":   ("manylinux_2_28_x86_64",),
    "windows": ("win_amd64",),
}

logger = logging.getLogger("refresh-ort-gpu")


# -----------------------------------------------------------------------------
# minimal HTTP helpers (stdlib only — no requests dep)

def http_get(url: str, accept: str = "*/*", timeout: int = 30) -> bytes:
    req = urllib.request.Request(url, headers={"User-Agent": "dt-ort-refresh", "Accept": accept})
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return r.read()


def http_get_json(url: str) -> object:
    return json.loads(http_get(url, accept="application/json").decode())


_FORMAT_MAGIC = {
    # detect that the bytes we got back actually look like the file
    # type we expect — guards against URLs that resolve to an HTML error
    # page served as 200 OK, or a redirect to a login wall
    "whl": b"PK\x03\x04",  # ZIP
    "zip": b"PK\x03\x04",
    "tgz": b"\x1f\x8b",    # gzip
}


def stream_sha256(url: str, fmt: str | None = None,
                  expected_size: int | None = None,
                  min_size: int = 1 << 20) -> tuple[str, int]:
    """Stream-download a URL and return (sha256_hex, byte_count).

    Validates the first bytes match `fmt`'s magic when given, and that
    the total size is at least `min_size` (1 MiB by default — anything
    smaller can't be one of our wheels/tarballs and is almost certainly
    an error page)."""
    h = hashlib.sha256()
    n = 0
    head: bytes = b""
    req = urllib.request.Request(url, headers={"User-Agent": "dt-ort-refresh"})
    with urllib.request.urlopen(req, timeout=120) as r:
        while True:
            chunk = r.read(1 << 20)  # 1 MiB
            if not chunk:
                break
            if not head:
                head = chunk[:8]
            h.update(chunk)
            n += len(chunk)
            if expected_size and n > expected_size * 1.5:
                raise RuntimeError(f"runaway download from {url} ({n} bytes)")
    if n < min_size:
        raise RuntimeError(
            f"{url}: got only {n} bytes (expected >={min_size}); "
            "URL likely returned an error page"
        )
    if fmt and (magic := _FORMAT_MAGIC.get(fmt)):
        if not head.startswith(magic):
            raise RuntimeError(
                f"{url}: first bytes {head!r} don't match {fmt} magic "
                f"{magic!r}; URL probably resolves to wrong content"
            )
    return h.hexdigest(), n


# -----------------------------------------------------------------------------
# Radeon directory scrape

class _LinkParser(HTMLParser):
    def __init__(self) -> None:
        super().__init__()
        self.hrefs: list[str] = []

    def handle_starttag(self, tag: str, attrs: list[tuple[str, str | None]]) -> None:
        if tag != "a":
            return
        for k, v in attrs:
            if k == "href" and v:
                self.hrefs.append(v)


def _list_links(url: str) -> list[str]:
    p = _LinkParser()
    p.feed(http_get(url).decode("utf-8", errors="replace"))
    return p.hrefs


_ROCM_DIR_RE = re.compile(r"^rocm-rel-(\d+)\.(\d+)(?:\.(\d+))?/$")
_ORT_WHL_RE = re.compile(
    r"^onnxruntime_(rocm|migraphx)-(\d+\.\d+\.\d+)-"
    rf"{PY_TAG}-{PY_TAG}-([\w._]+)\.whl$"
)


def discover_radeon_wheels() -> dict[tuple[int, int], dict]:
    """Return {(rocm_major, rocm_minor): {patch, ort_version, url, package}}.

    Only the *latest patch* in each minor is returned. URLs point at the
    real wheel observed in that patch's directory (filename tag may
    differ across patches, e.g. linux_x86_64 vs manylinux).
    """
    logger.info("scraping %s", RADEON_INDEX)
    rocm_dirs: dict[tuple[int, int], tuple[tuple[int, int, int], str]] = {}
    for href in _list_links(RADEON_INDEX):
        m = _ROCM_DIR_RE.match(href)
        if not m:
            continue
        major, minor = int(m.group(1)), int(m.group(2))
        # ROCm 6.x is not supported — known issues with the bundled libs
        if major < 7:
            continue
        patch = int(m.group(3)) if m.group(3) else 0
        sortkey = (major, minor, patch)
        prev = rocm_dirs.get((major, minor))
        if prev is None or sortkey > prev[0]:
            rocm_dirs[(major, minor)] = (sortkey, href.rstrip("/"))

    result: dict[tuple[int, int], dict] = {}
    for (major, minor), ((_, _, patch), dirname) in sorted(rocm_dirs.items()):
        dir_url = RADEON_INDEX + dirname + "/"
        try:
            files = _list_links(dir_url)
        except urllib.error.HTTPError as e:
            logger.warning("skipping %s: %s", dir_url, e)
            continue

        # find the cp312 ONNX Runtime wheel; prefer onnxruntime_migraphx (7.1+),
        # fall back to onnxruntime_rocm (7.0)
        best: tuple[str, str, str] | None = None  # (package, ver, fname)
        for fn in files:
            mw = _ORT_WHL_RE.match(fn)
            if not mw:
                continue
            pkg, ver = mw.group(1), mw.group(2)
            # prefer migraphx if both present
            if best is None or (best[0] == "rocm" and pkg == "migraphx"):
                best = (pkg, ver, fn)
        if best is None:
            logger.info("rocm %d.%d.%d: no cp312 ONNX Runtime wheel", major, minor, patch)
            continue
        pkg, ver, fname = best
        result[(major, minor)] = dict(
            patch=patch,
            ort_version=ver,
            package=pkg,
            url=dir_url + fname,
            filename=fname,
            requirements=f"ROCm {major}.{minor}, MIGraphX",
        )
        logger.info("rocm %d.%d.%d -> %s %s", major, minor, patch, pkg, ver)
    return result


# -----------------------------------------------------------------------------
# GitHub microsoft/onnxruntime releases scrape (NVIDIA)

def discover_cuda_assets() -> dict[tuple[str, int], dict]:
    """Return {(platform, cuda_major): {ort_version, url}} for the latest
    onnxruntime release that has all four expected assets present."""
    logger.info("querying %s", GH_RELEASES)
    headers = {"User-Agent": "dt-ort-refresh", "Accept": "application/vnd.github+json"}
    if (token := os.environ.get("GITHUB_TOKEN")):
        headers["Authorization"] = f"Bearer {token}"
    req = urllib.request.Request(GH_RELEASES + "?per_page=20", headers=headers)
    with urllib.request.urlopen(req, timeout=30) as r:
        releases = json.load(r)

    result: dict[tuple[str, int], dict] = {}
    for rel in releases:
        if rel.get("draft") or rel.get("prerelease"):
            continue
        ver = rel["tag_name"].lstrip("v")
        assets = {a["name"]: a["browser_download_url"] for a in rel.get("assets", [])}
        match: dict[tuple[str, int], dict] = {}
        for (plat, cuda_maj), tmpl in CUDA_PATTERNS.items():
            name = tmpl.format(ver=ver)
            if name not in assets:
                logger.info("rel %s missing %s", ver, name)
                break
            match[(plat, cuda_maj)] = dict(
                ort_version=ver, url=assets[name], requirements=f"CUDA {cuda_maj}.x, cuDNN 9.x",
            )
        if len(match) == len(CUDA_PATTERNS):
            logger.info("nvidia release %s has all assets", ver)
            return match
    raise RuntimeError("no recent onnxruntime release has the full CUDA asset set")


# -----------------------------------------------------------------------------
# registry diff

def proposed_amd(existing: list[dict], wheels: dict[tuple[int, int], dict]) -> list[dict]:
    """Build the AMD section of the new registry. Reuses entry shape +
    extra fields (size_mb, lib_pattern, install_subdir, required_libs)
    from the existing entries when present, so manual fields are kept."""
    new: list[dict] = []
    by_minor = {}
    for e in existing:
        if e.get("vendor") != "amd":
            continue
        try:
            mm = e["rocm_min"].split(".")
            by_minor[(int(mm[0]), int(mm[1]))] = e
        except (KeyError, ValueError):
            pass

    for (major, minor), info in sorted(wheels.items()):
        old = by_minor.get((major, minor), {})
        # tight per-minor range: bash matcher uses inclusive `>=`,
        # so overlapping ranges across minors would let `jq | first`
        # pick the wrong entry. one entry per ROCm minor — patches
        # match because the detector truncates to major.minor
        mm = f"{major}.{minor}"
        entry = {
            "vendor": "amd",
            "platform": "linux",
            "arch": "x86_64",
            "rocm_min": mm,
            "rocm_max": mm,
            "ort_version": info["ort_version"],
            "url": info["url"],
            # left empty here so fill_sha256 always considers this entry
            # for verification — pre-populating from `old` would make
            # the early-skip mistake an outdated registry SHA for a
            # PyPI-trusted one
            "sha256": "",
            "format": "whl",
            "lib_pattern": old.get("lib_pattern", "libonnxruntime"),
            "install_subdir": old.get("install_subdir", "onnxruntime-migraphx"),
            "size_mb": old.get("size_mb", 200),
            "requirements": info["requirements"],
        }
        if "required_libs" in old:
            entry["required_libs"] = old["required_libs"]
        new.append(entry)
    return new


def proposed_nvidia(existing: list[dict], assets: dict[tuple[str, int], dict]) -> list[dict]:
    new: list[dict] = []
    by_key = {}
    for e in existing:
        if e.get("vendor") != "nvidia":
            continue
        try:
            cm = int(float(e["cuda_min"]))
            by_key[(e["platform"], cm)] = e
        except (KeyError, ValueError):
            pass

    for (plat, cuda_maj), info in sorted(assets.items()):
        old = by_key.get((plat, cuda_maj), {})
        entry = {
            "vendor": "nvidia",
            "platform": plat,
            "arch": "x86_64",
            "cuda_min": f"{cuda_maj}.0",
            "cuda_max": f"{cuda_maj}.99",
            "ort_version": info["ort_version"],
            "url": info["url"],
            # see proposed_amd note: left empty so fill_sha256 always
            # verifies — never carry old SHA blindly
            "sha256": "",
            "format": "tgz" if plat == "linux" else "zip",
            "lib_pattern": old.get("lib_pattern", "libonnxruntime" if plat == "linux" else "onnxruntime"),
            "install_subdir": old.get("install_subdir", "onnxruntime-cuda"),
            "size_mb": old.get("size_mb", 200),
            "requirements": info["requirements"],
        }
        new.append(entry)
    return new


def fill_sha256(new_entries: list[dict], existing_entries: list[dict]) -> None:
    """Resolve sha256 for every entry that doesn't already carry one
    from a trusted upstream (PyPI's JSON API).

    Always downloads + hashes the wheel even when the URL matches an
    existing registry entry. Any cheaper shortcut (HEAD size check,
    Etag, etc.) misses drift between similar-sized versions: e.g. ORT
    1.24.4 and 1.25.1 are both ~210 MB so size is identical, but their
    SHAs differ. The only reliable way to know the cache is correct is
    to compute the SHA fresh.

    Cost: ~2 GB downloaded per run. For monthly CI that's negligible.
    Logs a warning when the stored SHA differs from upstream so drift
    in the source-of-truth registry is surfaced in CI output.
    """
    by_url = {e["url"]: e for e in existing_entries}
    for entry in new_entries:
        # Intel/PyPI entries arrive with sha256 already filled in from
        # PyPI's published digest — those are authoritative
        if entry.get("sha256"):
            continue

        logger.info("downloading %s for sha256...", entry["url"])
        sha, n = stream_sha256(entry["url"], fmt=entry.get("format"))
        entry["sha256"] = sha
        # round size up to nearest 50 MB for readability
        mb_actual = n / (1 << 20)
        entry["size_mb"] = max(50, ((int(mb_actual) + 49) // 50) * 50)

        old = by_url.get(entry["url"])
        if old and old.get("sha256") and old["sha256"] != sha:
            logger.warning(
                "%s: registry SHA %s does not match upstream %s — "
                "registry was drifted (manual edit, upstream rebuild, etc.)",
                entry["url"], old["sha256"][:12] + "...",
                sha[:12] + "...")


# -----------------------------------------------------------------------------
# PyPI scrape (Intel: onnxruntime-openvino + openvino runtime)

def _round_size_mb(byte_count: int, step_mb: int = 10) -> int:
    """Round byte_count up to the nearest step_mb, with a step floor."""
    mb = byte_count / (1 << 20)
    return max(step_mb, ((int(mb) + step_mb - 1) // step_mb) * step_mb)


def _pypi_pick_wheel(release_files: list[dict],
                     platform_tags: tuple[str, ...]) -> dict | None:
    """Pick the cp312 bdist_wheel matching one of the given platform tags."""
    for f in release_files:
        if f.get("packagetype") != "bdist_wheel":
            continue
        fn = f.get("filename", "")
        if PY_TAG not in fn:
            continue
        if not any(plat in fn for plat in platform_tags):
            continue
        return f
    return None


def discover_intel_assets() -> dict[str, dict]:
    """Return {platform: {ort_version, url, sha256, size_mb,
    [runtime_url, runtime_sha256, runtime_size_mb]}}.

    Latest cp312 onnxruntime_openvino wheel from PyPI per platform.
    Windows additionally pulls the matching openvino runtime wheel —
    on Linux openvino libs are bundled inside the ONNX Runtime wheel, on Windows
    they ship as a separate package."""
    logger.info("querying %s", PYPI_OPENVINO_ORT)
    data = http_get_json(PYPI_OPENVINO_ORT)
    ver = data["info"]["version"]
    files = data["releases"][ver]

    result: dict[str, dict] = {}
    for plat, tags in INTEL_WHEEL_TAGS.items():
        wheel = _pypi_pick_wheel(files, tags)
        if not wheel:
            logger.info("intel %s: no cp312 wheel in onnxruntime-openvino %s", plat, ver)
            continue
        result[plat] = dict(
            ort_version=ver,
            url=wheel["url"],
            sha256=wheel["digests"]["sha256"],
            size_mb=_round_size_mb(wheel["size"], 10),
        )

    if "windows" in result:
        logger.info("querying %s", PYPI_OPENVINO_RUNTIME)
        rt = http_get_json(PYPI_OPENVINO_RUNTIME)
        rt_ver = rt["info"]["version"]
        rt_wheel = _pypi_pick_wheel(rt["releases"][rt_ver],
                                    INTEL_WHEEL_TAGS["windows"])
        if rt_wheel:
            result["windows"]["runtime_url"] = rt_wheel["url"]
            result["windows"]["runtime_sha256"] = rt_wheel["digests"]["sha256"]
            result["windows"]["runtime_size_mb"] = _round_size_mb(rt_wheel["size"], 10)
        else:
            logger.warning("intel windows: openvino runtime wheel not found in %s", rt_ver)

    return result


def proposed_intel(existing: list[dict], assets: dict[str, dict]) -> list[dict]:
    """Build replacement intel entries from PyPI assets, preserving any
    existing static fields (lib_pattern, install_subdir, requirements,
    *_extra_patterns) that the discoverer can't infer."""
    by_plat = {e["platform"]: e for e in existing if e.get("vendor") == "intel"}
    new: list[dict] = []
    for plat, info in sorted(assets.items()):
        old = by_plat.get(plat, {})
        entry: dict = {
            "vendor": "intel",
            "platform": plat,
            "arch": "x86_64",
            "ort_version": info["ort_version"],
            "url": info["url"],
            "sha256": info["sha256"],
            "format": "whl",
            "lib_pattern": old.get("lib_pattern",
                                   "libonnxruntime" if plat == "linux" else "onnxruntime"),
        }
        if "lib_extra_patterns" in old:
            entry["lib_extra_patterns"] = old["lib_extra_patterns"]
        entry["install_subdir"] = old.get("install_subdir", "onnxruntime-openvino")
        entry["size_mb"] = info["size_mb"]
        entry["requirements"] = old.get("requirements", "Intel GPU driver (OpenCL)")
        if plat == "windows" and "runtime_url" in info:
            entry["runtime_url"] = info["runtime_url"]
            entry["runtime_sha256"] = info["runtime_sha256"]
            entry["runtime_lib_pattern"] = old.get("runtime_lib_pattern", "openvino")
            if "runtime_extra_patterns" in old:
                entry["runtime_extra_patterns"] = old["runtime_extra_patterns"]
            entry["runtime_size_mb"] = info["runtime_size_mb"]
        new.append(entry)
    return new


# -----------------------------------------------------------------------------
# registry I/O

def load_registry() -> dict:
    return json.loads(REGISTRY_PATH.read_text())


def write_registry(data: dict) -> None:
    REGISTRY_PATH.write_text(json.dumps(data, indent=2) + "\n")


def render_diff(old: dict, new: dict) -> str:
    import difflib
    a = json.dumps(old, indent=2).splitlines(keepends=True)
    b = json.dumps(new, indent=2).splitlines(keepends=True)
    return "".join(difflib.unified_diff(a, b, fromfile="ort_gpu.json (current)", tofile="ort_gpu.json (proposed)"))


# -----------------------------------------------------------------------------
# main

def build_proposed(existing: dict) -> dict:
    """Build the proposed registry. Computes sha256 only for changed URLs.
    Preserves any vendor we don't manage untouched."""
    pkgs = list(existing.get("packages", []))
    nvidia = proposed_nvidia(pkgs, discover_cuda_assets())
    amd = proposed_amd(pkgs, discover_radeon_wheels())
    intel = proposed_intel(pkgs, discover_intel_assets())
    preserved = [e for e in pkgs if e.get("vendor") not in {"nvidia", "amd", "intel"}]
    new_pkgs = nvidia + amd + intel + preserved
    fill_sha256(new_pkgs, pkgs)
    out = dict(existing)
    out["packages"] = new_pkgs
    return out


def open_pr(diff: str) -> None:
    branch = f"refresh-ort-gpu-{os.environ.get('GITHUB_RUN_ID', 'manual')}"
    subprocess.check_call(["git", "checkout", "-b", branch])
    subprocess.check_call(["git", "add", str(REGISTRY_PATH)])
    subprocess.check_call(["git", "commit", "-m", "refresh ort_gpu.json from upstream"])
    subprocess.check_call(["git", "push", "-u", "origin", branch])
    body = (
        "Automated update from `tools/ai/refresh-ort-gpu.py`.\n\n"
        "```diff\n" + diff + "\n```"
    )
    subprocess.check_call(["gh", "pr", "create", "--title", "Refresh ONNX Runtime GPU registry", "--body", body])


def main(argv: list[str]) -> int:
    p = argparse.ArgumentParser(description=__doc__)
    mode = p.add_mutually_exclusive_group()
    mode.add_argument("--check", action="store_true", help="dry-run; print diff and exit non-zero if changes")
    mode.add_argument("--update", action="store_true", help="rewrite data/ort_gpu.json in place")
    p.add_argument("--pr", action="store_true", help="open a PR with the changes (implies --update)")
    p.add_argument("-v", "--verbose", action="store_true")
    args = p.parse_args(argv)

    logging.basicConfig(level=logging.INFO if args.verbose or args.check else logging.WARNING,
                        format="%(message)s")

    if args.pr and not args.update:
        args.update = True
    if not (args.check or args.update):
        args.check = True  # default

    existing = load_registry()
    proposed = build_proposed(existing)

    if proposed == existing:
        print("ort_gpu.json is up to date.")
        return 0

    diff = render_diff(existing, proposed)
    print(diff)

    if args.update:
        write_registry(proposed)
        print(f"\nwrote {REGISTRY_PATH}")
    if args.pr:
        open_pr(diff)
    return 1 if args.check else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
