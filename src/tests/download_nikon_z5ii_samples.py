#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 darktable developers

"""Download the CC0 Nikon regression samples and verify their SHA-256 hashes."""

import argparse
import hashlib
import json
from pathlib import Path
import urllib.request


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    parser.add_argument("--codestreams", type=Path,
                        help="also copy HE codestreams into a libFuzzer seed directory")
    args = parser.parse_args()
    args.directory.mkdir(parents=True, exist_ok=True)
    if args.codestreams:
        args.codestreams.mkdir(parents=True, exist_ok=True)
    manifest = json.loads(Path(__file__).with_name("nikon_z5ii_samples.json").read_text())
    for sample in manifest["samples"]:
        path = args.directory / (sample["name"] + ".NEF")
        if not path.exists():
            temporary = path.with_suffix(".partial")
            try:
                with urllib.request.urlopen(sample["url"], timeout=120) as response:
                    with temporary.open("wb") as output:
                        while chunk := response.read(1024 * 1024):
                            output.write(chunk)
                if hashlib.sha256(temporary.read_bytes()).hexdigest() != sample["sha256"]:
                    raise ValueError(f"SHA-256 mismatch: {path.name}")
                temporary.replace(path)
            finally:
                temporary.unlink(missing_ok=True)
        data = path.read_bytes()
        if hashlib.sha256(data).hexdigest() != sample["sha256"]:
            raise ValueError(f"SHA-256 mismatch: {path.name}")
        if args.codestreams and sample["mode"] != "lossless":
            start = sample["codestream_offset"]
            size = sample["codestream_size"]
            stream = data[start:start + size]
            if len(stream) != size or stream[:4] != b"\xff\x10\xff\x50":
                raise ValueError(f"invalid codestream range: {path.name}")
            (args.codestreams / sample["name"]).write_bytes(stream)
        print(f"verified {path.name}", flush=True)


if __name__ == "__main__":
    main()
