#!/usr/bin/env bash
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

# Compares the pixel hash of each test's output.png with the reference
# hashes for the CPU vendor of this machine, in <refs>/<vendor_id>.txt
# ("<test> <hash>..." lines, # starts a comment). Built without
# target_clones (DT_NO_CLONE_TARGETS), most outputs are exactly the same
# on every machine of a vendor; a test whose output varies from run to
# run lists every output seen.
#
# An output among its reference hashes is the reference output, whatever
# its Delta-E against expected.png, which was rendered on another machine.
# Any other output is left to ./run's Delta-E check.
#
# usage: integration-hashes.sh <refs dir> <hashes out> <test>...
#   run from src/tests/integration after ./run; writes "<test> <hash>"
#   for every test to <hashes out>, the lines to paste into the reference
#   file when an output changes on purpose. Prints a "<test>: ..." line
#   for each test whose output is not among its reference hashes.

set -uo pipefail

refs="$1" out="$2"
shift 2
vendor=$(awk -F': *' '/^vendor_id/ { print $2; exit }' /proc/cpuinfo)
model=$(awk -F': *' '/^model name/ { print $2; exit }' /proc/cpuinfo)
ref="$refs/$vendor.txt"
[ -f "$ref" ] || echo "no reference file for CPU vendor '$vendor'"

: > "$out"
differ=()
for test in "$@"; do
  hash=missing
  [ -f "$test/output.png" ] && hash=$(identify -format '%#' "$test/output.png" 2> /dev/null || echo unreadable)
  echo "$test $hash" >> "$out"
  expected=
  [ -f "$ref" ] && expected=$(sed 's/#.*//' "$ref" | awk -v t="$test" '$1 == t { $1 = ""; print }')
  if [ -z "$expected" ]; then
    echo "$test: no reference hash for $vendor"
  elif [[ " $expected " != *" $hash "* ]]; then
    echo "$test: $hash, not in the references"
    differ+=("$test")
  fi
done

echo "$vendor ($model): $# tests, ${#differ[@]} with a changed output"
