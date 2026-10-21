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

# Lists the integration tests that CI would run for a commit or a group of
# commits, with the same selection as the integration workflow
# (.ci/integration-select.py).
#
# usage: tools/integration-tests-for.sh <commit>|<from>..<to>|<from>...<to> ...
#   a commit alone is compared with its first parent; a range like the
#   workflow does, from the merge base; several arguments add up
#   -q prints the test names only

set -euo pipefail

quiet=
[ "${1:-}" = -q ] && { quiet=1; shift; }
[ $# -gt 0 ] || { sed -n 's/^# \{0,1\}//; /^usage:/,/^$/p' "$0"; exit 2; }

cd "$(git rev-parse --show-toplevel)"
suite=src/tests/integration
ls "$suite"/[0-9]*/*.xmp > /dev/null 2>&1 \
  || { echo "$suite has no tests: git submodule update --init $suite" >&2; exit 1; }

changed=$(for arg in "$@"; do
  case "$arg" in
    *...*|*..*) from=${arg%%..*} to=${arg##*..}
                git diff --name-only "$(git merge-base "$from" "${to:-HEAD}")" "${to:-HEAD}" ;;
    *)          git diff --name-only "$arg^1" "$arg" ;;
  esac
done | sort -u)

selection=$(printf '%s\n' "$changed" | python3 .ci/integration-select.py "$suite" 4)

if [ -n "$quiet" ]; then
  python3 -c 'import json, sys; print("\n".join(json.load(sys.stdin)["tests"]))' <<< "$selection"
else
  echo "$(printf '%s\n' "$changed" | grep -c .) changed files"
  python3 -c '
import json, sys
d = json.load(sys.stdin)
for reason in d["reason"].split("; "):
  print("  " + reason)
print("%d tests, %d shards" % (len(d["tests"]), len(d["shards"])))
for t in d["tests"]:
  print("  " + t)' <<< "$selection"
fi
