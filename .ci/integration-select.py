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
#   - a processing module's source (src/iop/<op>.*, its OpenCL kernel
#     data/kernels/<op>.cl, or a file only that module uses) selects the
#     tests whose history uses <op>;
#   - code with no visible effect on the pixels (the GUI, Lua, the unit
#     tests) selects nothing;
#   - anything else under src/, data/kernels or the build files can affect
#     any test, and selects the whole suite.
#
# usage: integration-select.py <integration dir> <max shards> < changed-files
#   prints {"tests": [...], "shards": ["<tests>", ...], "reason": "..."}

import glob
import json
import os
import re
import sys

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

# files of src/iop/ that belong to a single module without being named after it
MODULE_OF = [
  (r"^src/iop/demosaicing/", "demosaic"),
  (r"^src/iop/hlreconstruct/", "highlights"),
  (r"^src/iop/ashift_[^/]*$", "ashift"),
  (r"^src/iop/lut3dgmic\.cpp$", "lut3d"),
]
# no visible effect on the pixels of darktable-cli's output
NO_PIXELS = r"^(src/(libs|views|gui|dtgtk|bauhaus|lua|tests/unittests)/" \
            r"|po/|doc/|data/(themes|lua|pixmaps|style|watermarks|latex|pswp)/|tools/|packaging/)" \
            r"|\.md$"


def module_of(path):
  for pattern, op in MODULE_OF:
    if re.search(pattern, path):
      return op
  m = re.match(r"^src/iop/([^/]+)\.(c|cc|cpp)$", path) or re.match(r"^data/kernels/([^/]+)\.cl$", path)
  if m and (os.path.exists(f"src/iop/{m.group(1)}.c") or os.path.exists(f"src/iop/{m.group(1)}.cc")):
    return m.group(1)
  return None


selected, reasons, everything = set(), [], None
for path in changed:
  if re.search(NO_PIXELS, path):
    continue
  op = module_of(path)
  if op is not None:
    tests = uses.get(op, set())
    selected |= tests
    reasons.append(f"{path}: {op}, {len(tests)} tests")
  elif path.startswith(("src/", "data/kernels/", "cmake/")) or os.path.basename(path) == "CMakeLists.txt" \
       or path.startswith((".github/workflows/integration.yml", ".ci/integration-select.py")):
    everything = path
    break

if everything:
  selected = set(all_tests)
  reasons = [f"{everything}: may affect any test, the whole suite"]
tests = sorted(selected)

# round-robin over the sorted list, up to ~45 tests (~7 min on a runner) a shard
n = min(max_shards, max(1, -(-len(tests) // 45))) if tests else 0
shards = [" ".join(tests[i::n]) for i in range(n)]
json.dump({"tests": tests, "shards": shards, "reason": "; ".join(reasons) or "no change with an effect on the pixels"},
          sys.stdout)
print()
