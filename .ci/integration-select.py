#!/usr/bin/env python3
#
#    This file is part of darktable.
#    Copyright (C) 2026 darktable developers.
#
#    darktable is free software: you can redistribute it and/or modify
#    it under the terms of the GNU General Public License as published by
#    the Free Software Foundation, either version 3 of the License, or
#    (at your option) any later version.
#
#    darktable is distributed in the hope that it will be useful,
#    but WITHOUT ANY WARRANTY; without even the implied warranty of
#    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
#    GNU General Public License for more details.
#
#    You should have received a copy of the GNU General Public License
#    along with darktable.  If not, see <http://www.gnu.org/licenses/>.

# Selects the integration tests worth running for a set of changed files,
# and splits them into shards for parallel jobs.
#
#   - a processing module's source (a file add_iop() lists in
#     src/iop/CMakeLists.txt, its OpenCL kernel data/kernels/<op>.cl, or a
#     file only that module includes) selects the tests whose history uses
#     <op>;
#   - a module no history names (finalscale, which the pixelpipe inserts by
#     itself) selects the whole suite: there is no telling which tests run
#     it. A module that runs by default without being in some histories
#     (highlights) is only caught there by a full run;
#   - code with no visible effect on the pixels (the GUI, Lua, the unit
#     tests) selects nothing;
#   - anything else under src/, data/kernels or the build files can affect
#     any test, and selects the whole suite.
#
# A test marked critical runs whatever changed: its directory holds a
# ci.toml with
#
#   critical = true
#
# usage: integration-select.py <integration dir> <max shards> < changed-files
#   prints {"tests": [...], "shards": ["<tests>", ...], "reason": "..."}

import glob
import json
import os
import re
import sys
import tomllib

suite, max_shards = sys.argv[1], int(sys.argv[2])
changed = [line.strip() for line in sys.stdin if line.strip()]

# operation -> tests whose history uses it
uses = {}
for xmp in glob.glob(os.path.join(suite, "[0-9]*", "*.xmp")):
  test = os.path.basename(os.path.dirname(xmp))
  with open(xmp, errors="replace") as f:
    for op in set(re.findall(r'darktable:operation="([^"]+)"', f.read())):
      uses.setdefault(op, set()).add(test)
all_tests = sorted({t for ts in uses.values() for t in ts})

critical = set()
for meta in glob.glob(os.path.join(suite, "[0-9]*", "ci.toml")):
  with open(meta, "rb") as f:
    if tomllib.load(f).get("critical") is True:
      critical.add(os.path.basename(os.path.dirname(meta)))

# the sources of each module, as add_iop(<op> "<source>"...) lists them;
# the operation name is not always the file's (colorreconstruct)
SOURCES_OF = {}
with open("src/iop/CMakeLists.txt") as f:
  for op, sources in re.findall(r'add_iop\s*\(\s*(\w+)((?:\s+"[^"]+")+)', f.read()):
    for source in re.findall(r'"([^"]+)"', sources):
      SOURCES_OF.setdefault(os.path.normpath(os.path.join("src/iop", source)), set()).add(op)
OPS = {op for ops in SOURCES_OF.values() for op in ops}

# files of src/iop/ that the module's source #includes
INCLUDED_BY = [
  (r"^src/iop/demosaicing/", "demosaic"),
  (r"^src/iop/hlreconstruct/", "highlights"),
  (r"^src/iop/ashift_[^/]*$", "ashift"),
]
# no visible effect on the pixels of darktable-cli's output
NO_PIXELS = r"^(src/(libs|views|gui|dtgtk|bauhaus|lua|tests/unittests|osx|win|mcp)/|src/main\.c$" \
            r"|po/|doc/|data/(themes|lua|pixmaps|style|watermarks|latex|pswp)/|tools/|packaging/)" \
            r"|\.md$"


def modules_of(path):
  for pattern, op in INCLUDED_BY:
    if re.search(pattern, path):
      return {op}
  if path in SOURCES_OF:
    return SOURCES_OF[path]
  m = re.match(r"^data/kernels/([^/]+)\.cl$", path)
  if m and m.group(1) in OPS:
    return {m.group(1)}
  return None


selected, reasons, everything = set(), [], None
for path in changed:
  if re.search(NO_PIXELS, path):
    continue
  ops = modules_of(path)
  if ops and not any(op in uses for op in ops):
    everything = path
    break
  if ops is not None:
    tests = set().union(*(uses.get(op, set()) for op in ops))
    selected |= tests
    reasons.append(f"{path}: {','.join(sorted(ops))}, {len(tests)} tests")
  elif path.startswith(("src/", "data/kernels/", "cmake/")) or os.path.basename(path) == "CMakeLists.txt" \
       or path.startswith((".github/workflows/integration.yml", ".ci/integration-select.py")):
    everything = path
    break

if everything:
  selected = set(all_tests)
  reasons = [f"{everything}: may affect any test, the whole suite"]
if critical - selected:
  reasons.append(f"{len(critical - selected)} critical tests")
tests = sorted(selected | critical)

# round-robin over the sorted list, up to ~45 tests (~7 min on a runner) a shard
n = min(max_shards, max(1, -(-len(tests) // 45))) if tests else 0
shards = [" ".join(tests[i::n]) for i in range(n)]
json.dump({"tests": tests, "shards": shards, "reason": "; ".join(reasons) or "no change with an effect on the pixels"},
          sys.stdout)
print()
