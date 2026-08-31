#!/usr/bin/env python3
"""Aggregate and deduplicate sanitizer reports from a log directory.

Reads the per-process files a sanitizer run leaves behind and collapses them
into a ranked list of distinct findings.  Two kinds of file are picked up:

  <runtime>.<pid>  written by the sanitizers' log_path= option
  stderr.<pid>     captured by the darktable-cli shim in
                   tools/run-integration-tests.sh -- this is where GCC's UBSan
                   puts its non-fatal "runtime error:" diagnostics, which it
                   prints straight to stderr regardless of log_path

Two reports are considered the same finding when their (address-normalized)
headline and their first three non-runtime stack frames match.

Exits 1 when at least one report was found, 0 when the directory is clean, so
it can be used directly as a check.  Usable standalone on an old log dir:

    tools/sanitizer/aggregate-reports.py <build>/sanitizer-logs/<stamp>
"""

import argparse
import collections
import glob
import os
import re
import sys

LOG_PREFIXES = ("asan", "ubsan", "lsan", "tsan", "stderr")

# One regex per report headline shape.
HEADERS = (
    re.compile(r"^==\d+==ERROR: (?P<kind>\w+Sanitizer): (?P<what>[^\n]*)"),
    re.compile(r"^==\d+==WARNING: (?P<kind>ThreadSanitizer): (?P<what>[^\n]*)"),
    # LeakSanitizer emits one of these per distinct allocation site.
    re.compile(r"^(?P<kind>Direct leak|Indirect leak) of (?P<what>[^\n]*)"),
    re.compile(r"^(?P<file>[^\s:]+):(?P<line>\d+):\d+: runtime error: (?P<what>[^\n]*)"),
)

# The trailing SUMMARY line restates the ERROR line above it, and the
# LeakSanitizer banner is followed by the "Direct/Indirect leak of ..." entries
# that carry the actual stack traces.  Skipping both avoids double counting.
SKIP = re.compile(
    r"^(SUMMARY: \w+Sanitizer:|==\d+==ERROR: LeakSanitizer: detected memory leaks)"
)

FRAME = re.compile(r"^\s*#(\d+) 0x[0-9a-f]+ in (?P<sym>\S+)(?: (?P<loc>\S+))?")

# Frames inside the sanitizer runtime itself say nothing about our bug.
NOISE = re.compile(
    r"(libsanitizer|sanitizer_common|asan_|ubsan_|tsan_|lsan_|"
    r"interception|__interceptor|libasan|libubsan|libtsan|liblsan)"
)

FRAMES_PER_SIGNATURE = 3


def report_files(log_dir):
    files = []
    for prefix in LOG_PREFIXES:
        files.extend(glob.glob(os.path.join(log_dir, prefix + ".*")))
    return sorted(files)


def headline(match):
    groups = match.groupdict()
    if groups.get("file"):
        return "runtime error: %s (%s:%s)" % (
            groups["what"],
            groups["file"],
            groups["line"],
        )
    return "%s: %s" % (groups.get("kind", "?"), groups.get("what", "").strip())


def collect_frames(lines, start):
    """Return the first few meaningful frames after a headline, and where they end."""
    frames = []
    i = start
    while i < len(lines) and len(frames) < FRAMES_PER_SIGNATURE:
        match = FRAME.match(lines[i])
        if match:
            symbol = match.group("sym")
            location = match.group("loc") or ""
            if not NOISE.search(symbol) and not NOISE.search(location):
                frames.append("%s %s" % (symbol, location))
        elif not lines[i].strip() and frames:
            break
        i += 1
    return frames, i


def signature(title, frames):
    """Normalize away the parts that differ between occurrences of one bug."""
    normalized = re.sub(r"0x[0-9a-f]+", "0xADDR", title)
    if normalized.startswith(("Direct leak", "Indirect leak")):
        # The byte and object counts vary run to run.
        normalized = re.sub(r"\d+", "N", normalized)
    return normalized, tuple(frames)


def parse(path):
    """Yield (normalized title, frames) for every report in one file."""
    try:
        with open(path, errors="replace") as handle:
            lines = handle.read().splitlines()
    except OSError:
        return

    i = 0
    while i < len(lines):
        line = lines[i]
        if SKIP.match(line):
            i += 1
            continue

        match = next((m for m in (rx.match(line) for rx in HEADERS) if m), None)
        if not match:
            i += 1
            continue

        frames, i = collect_frames(lines, i + 1)
        yield signature(headline(match), frames)


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("log_dir", help="directory holding the sanitizer log files")
    parser.add_argument("-o", "--output", help="also write the summary to this file")
    args = parser.parse_args()

    files = report_files(args.log_dir)

    counts = collections.Counter()
    for path in files:
        for finding in parse(path):
            counts[finding] += 1

    total = sum(counts.values())

    lines = [
        "Sanitizer report summary",
        "=" * 72,
        "log dir      : %s" % args.log_dir,
        "report files : %d" % len(files),
        "raw reports  : %d" % total,
        "unique       : %d" % len(counts),
        "",
    ]
    for (title, frames), count in counts.most_common():
        lines.append("[%4dx] %s" % (count, title))
        lines.extend("         %s" % frame for frame in frames)
        lines.append("")

    text = "\n".join(lines)
    print(text)
    if args.output:
        with open(args.output, "w") as handle:
            handle.write(text + "\n")

    return 1 if total else 0


if __name__ == "__main__":
    sys.exit(main())
