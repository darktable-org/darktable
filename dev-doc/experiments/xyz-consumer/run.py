#!/usr/bin/env python3
"""Build and run the standalone XYZ consumer comparison using a native Ninja build."""

import argparse
import json
from pathlib import Path
import shlex
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("build", type=Path)
    parser.add_argument("--threads", type=int, default=1)
    parser.add_argument("--pixels", type=int, default=131072)
    args = parser.parse_args()
    if args.threads < 1 or not 12 <= args.pixels <= 4194304:
        parser.error("use positive threads and 12 through 4194304 pixels")

    root = Path(__file__).resolve().parent
    repo = root.parents[2]
    build = args.build.resolve()
    entries = json.loads((build / "compile_commands.json").read_text())
    source = repo / "src/tests/unittests/common/test_iop_xyz.c"
    entry = next((e for e in entries if Path(e["file"]).resolve() == source), None)
    if entry is None:
        parser.error("build this checkout with BUILD_TESTING and compile commands enabled")

    output = build / "xyz-consumer-experiment"
    output.mkdir(exist_ok=True)
    obj = output / "consumer.o"
    executable = output / "consumer"
    command = shlex.split(entry["command"])
    command[command.index("-c") + 1] = str(root / "consumer.c")
    command[command.index("-o") + 1] = str(obj)
    subprocess.run(command, cwd=entry["directory"], check=True)

    commands = subprocess.check_output(
        ["ninja", "-t", "commands", "test_iop_xyz"], cwd=build, text=True
    )
    link = shlex.split(commands.strip().splitlines()[-1])
    while link and link[0] in (":", "&&"):
        link.pop(0)
    if "&&" in link:
        link = link[:link.index("&&")]
    link[link.index("-o") + 1] = str(executable)
    original_objects = [i for i, v in enumerate(link) if v.endswith("/test_iop_xyz.c.o")]
    if len(original_objects) != 1:
        parser.error("cannot identify the native test object in the Ninja link command")
    link[original_objects[0]] = str(obj)
    subprocess.run(link, cwd=build, check=True)
    subprocess.run([str(executable), str(args.threads), str(args.pixels)], check=True)


if __name__ == "__main__":
    main()
