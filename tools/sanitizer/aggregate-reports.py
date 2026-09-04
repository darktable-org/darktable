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

Start-up failures are counted and reported separately. A runtime that dies
before main() instruments nothing, so "no findings" from such a run means "no
coverage", which is the opposite of a clean result.

Exit status: 0 when the directory is clean, 1 when there are findings, 2 when a
sanitizer failed to start.  Usable standalone on an old log dir:

    tools/sanitizer/aggregate-reports.py <build>/sanitizer-logs/<stamp>
"""

import argparse
import collections
import os
import re
import sys
import textwrap

# tools/run-integration-tests.sh files every report under a directory named
# after the test that produced it, and tags the name with the pass it came from,
# so a whole-suite run stays traceable: <log dir>/0004-masks/ubsan-cpu.12345
REPORT_FILE = re.compile(r"^(?:asan|ubsan|lsan|tsan|stderr)(?:-[a-z]+)?\.\d+$")

# A sanitizer that cannot lay out its shadow memory dies here, before any of
# the program runs. Nearly always the kernel's ASLR entropy is higher than the
# runtime can cope with: kernels default vm.mmap_rnd_bits to 32 while GCC's
# runtimes expect at most 28. Matched before HEADERS, because the ASan spelling
# also satisfies the generic ERROR pattern below.
FATAL = re.compile(
    r"^(?:==\d+==)?(?:FATAL|ERROR): (?P<kind>\w+Sanitizer): "
    r"(?P<what>(?:unexpected memory mapping"
    r"|Shadow memory range interleaves"
    r"|failed to allocate"
    r"|unable to mmap)[^\n]*)"
)

# One regex per report headline shape. The "==<pid>==" prefix is an ASan-ism:
# ThreadSanitizer writes a bare "WARNING: ..." under a "====" separator line and
# carries the pid in a trailing "(pid=N)" instead, so the prefix is optional.
HEADERS = (
    re.compile(r"^(?:==\d+==)?ERROR: (?P<kind>\w+Sanitizer): (?P<what>[^\n]*)"),
    re.compile(r"^(?:==\d+==)?WARNING: (?P<kind>ThreadSanitizer): (?P<what>[^\n]*)"),
    # LeakSanitizer emits one of these per distinct allocation site.
    re.compile(r"^(?P<kind>Direct leak|Indirect leak) of (?P<what>[^\n]*)"),
    re.compile(r"^(?P<file>[^\s:]+):(?P<line>\d+):\d+: runtime error: (?P<what>[^\n]*)"),
)

# The trailing SUMMARY line restates the ERROR line above it, and the
# LeakSanitizer banner is followed by the "Direct/Indirect leak of ..." entries
# that carry the actual stack traces.  Skipping both avoids double counting.
SKIP = re.compile(
    r"^(SUMMARY: \w+Sanitizer:|(?:==\d+==)?ERROR: LeakSanitizer: detected memory leaks)"
)

# Frame layouts differ between runtimes. clang and ASan print the address and an
# "in" separator:
#   #1 0x55f4 in dt_dev_pixelpipe_process src/develop/pixelpipe_hb.c:2094
# GCC's TSan drops both and appends the module the frame resolved to:
#   #1 g_socket_send_message <null> (libgio-2.0.so.0+0xa5c13) (BuildId: ...)
FRAME = re.compile(
    r"^\s*#\d+ "
    r"(?:0x[0-9a-f]+ in )?"
    r"(?P<sym>\S+)"
    r"(?: (?P<loc>\S+))?"
    r"(?: \((?P<mod>[^()\s]+\+0x[0-9a-f]+)\))?"
)

# Frames inside the sanitizer runtime itself say nothing about our bug.
NOISE = re.compile(
    r"(libsanitizer|sanitizer_common|asan_|ubsan_|tsan_|lsan_|"
    r"interception|__interceptor|libasan|libubsan|libtsan|liblsan)"
)

FRAMES_PER_SIGNATURE = 3


def report_files(log_dir):
    """Every per-process report below log_dir, including the per-test subdirs."""
    files = []
    for root, dirs, names in os.walk(log_dir):
        dirs.sort()
        files.extend(os.path.join(root, name)
                     for name in sorted(names) if REPORT_FILE.match(name))
    return files


def origin(log_dir, path):
    """Which test a report belongs to, from the directory the shim filed it in."""
    relative = os.path.relpath(os.path.dirname(path), log_dir)
    return "(run)" if relative == "." else relative


def headline(match):
    groups = match.groupdict()
    if groups.get("file"):
        return "runtime error: %s (%s:%s)" % (
            groups["what"],
            groups["file"],
            groups["line"],
        )
    return "%s: %s" % (groups.get("kind", "?"), groups.get("what", "").strip())


def frame_text(match):
    """Render one frame, falling back to the module when symbols are missing.

    GCC prints "<null>" for both symbol and file when a library has no debug
    info; the module and offset are then the only identifying part left, and
    they are stable across runs because the offset is module-relative.
    """
    parts = [
        part
        for part in (match.group("sym"), match.group("loc"))
        if part and part != "<null>"
    ]
    return " ".join(parts) if parts else (match.group("mod") or "")


def collect_frames(lines, start):
    """Return the first few meaningful frames after a headline, and where they end."""
    frames = []
    i = start
    while i < len(lines) and len(frames) < FRAMES_PER_SIGNATURE:
        match = FRAME.match(lines[i])
        if match:
            text = frame_text(match)
            if text and not NOISE.search(text):
                frames.append(text)
        elif not lines[i].strip() and frames:
            break
        i += 1
    return frames, i


def normalize(title):
    """Strip the parts that differ between occurrences of one finding."""
    title = re.sub(r"\s*\(pid=\d+\)", "", title)
    return re.sub(r"0x[0-9a-f]+", "0xADDR", title)


def signature(title, frames):
    normalized = normalize(title)
    if normalized.startswith(("Direct leak", "Indirect leak")):
        # The byte and object counts vary run to run.
        normalized = re.sub(r"\d+", "N", normalized)
    return normalized, tuple(frames)


def parse(path):
    """Yield ("fatal", title) or ("finding", (title, frames)) for one file."""
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

        fatal = FATAL.match(line)
        if fatal:
            yield "fatal", normalize(headline(fatal))
            i += 1
            continue

        match = next((m for m in (rx.match(line) for rx in HEADERS) if m), None)
        if not match:
            i += 1
            continue

        frames, i = collect_frames(lines, i + 1)
        yield "finding", signature(headline(match), frames)


WIDTH = 79


def describe(where, prefix):
    """List every test a finding turned up in, wrapped onto continuation lines.

    Deliberately not truncated: which tests are affected, and which are not, is
    the thing you are reading the summary for. break_on_hyphens stays off so
    that a name like 0002-local-contrast is never split across two lines.
    """
    return textwrap.wrap(", ".join(sorted(where)),
                         width=WIDTH,
                         initial_indent=prefix,
                         subsequent_indent=" " * len(prefix),
                         break_long_words=False,
                         break_on_hyphens=False)


STARTUP_HINT = """\
   A runtime that cannot map its shadow memory almost always means the kernel's
   ASLR entropy is higher than it supports. Either lower it system-wide:
       sudo sysctl -w vm.mmap_rnd_bits=28
   or run the binary with ASLR off:
       setarch -R <binary>
   tools/run-integration-tests.sh detects this and applies setarch itself."""


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("log_dir", help="directory holding the sanitizer log files")
    parser.add_argument("-o", "--output", help="also write the summary to this file")
    args = parser.parse_args()

    files = report_files(args.log_dir)

    counts = collections.Counter()
    fatals = collections.Counter()
    # Which tests each finding was seen in. The same defect usually fires in
    # many tests, and knowing whether it is one test or all of them is most of
    # the triage.
    seen_in = collections.defaultdict(set)
    tests = set()
    for path in files:
        where = origin(args.log_dir, path)
        for kind, item in parse(path):
            if kind == "fatal":
                fatals[item] += 1
            else:
                counts[item] += 1
                seen_in[item].add(where)
            tests.add(where)

    total = sum(counts.values())

    lines = [
        "Sanitizer report summary",
        "=" * 72,
        "log dir      : %s" % args.log_dir,
        "report files : %d" % len(files),
        "tests        : %d" % len(tests),
        "raw reports  : %d" % total,
        "unique       : %d" % len(counts),
        "",
    ]

    if fatals:
        lines.append(
            "!! %d sanitizer start-up failure(s) -- this run produced NO coverage"
            % sum(fatals.values())
        )
        for title, count in fatals.most_common():
            lines.append("   [%4dx] %s" % (count, title))
        lines.append(STARTUP_HINT)
        lines.append("")

    for finding, count in counts.most_common():
        title, frames = finding
        lines.append("[%4dx] %s" % (count, title))
        lines.extend("         %s" % frame for frame in frames)
        lines.extend(describe(seen_in[finding], "         in: "))
        lines.append("")

    text = "\n".join(lines)
    print(text)
    if args.output:
        with open(args.output, "w") as handle:
            handle.write(text + "\n")

    if fatals:
        return 2
    return 1 if total else 0


if __name__ == "__main__":
    sys.exit(main())
