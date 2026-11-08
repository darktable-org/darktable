#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail

usage() {
  echo "Usage: $0 --ndk /absolute/path/to/ndk --output /absolute/path/to/fresh-directory"
}

ndk=
output=
while (($#)); do
  case "$1" in
    --ndk|--output)
      (($# >= 2)) || { usage >&2; exit 1; }
      if [[ "$1" == --ndk ]]; then ndk=$2; else output=$2; fi
      shift 2
      ;;
    --help|-h) usage; exit 0 ;;
    *) usage >&2; exit 1 ;;
  esac
done
[[ "$ndk" == /* && "$output" == /* ]] || { usage >&2; exit 1; }
[[ ! -e "$output" && ! -L "$output" ]] || {
  echo "Output already exists; choose a fresh directory: $output" >&2; exit 1;
}
[[ -f "$ndk/source.properties" ]] || { echo "NDK source.properties is missing" >&2; exit 1; }
revision=$(sed -n 's/^Pkg.Revision[[:space:]]*=[[:space:]]*//p' "$ndk/source.properties" | tr -d '\r')
[[ "$revision" == 27.2.12479018 ]] || {
  echo "Expected Android NDK 27.2.12479018, got $revision" >&2; exit 1;
}
case "$(uname -s)" in
  Linux) host_tag=linux-x86_64 ;;
  Darwin) host_tag=darwin-x86_64 ;;
  *) echo "Unsupported NDK build host" >&2; exit 1 ;;
esac
toolchain="$ndk/toolchains/llvm/prebuilt/$host_tag"
compiler="$toolchain/bin/aarch64-linux-android26-clang"
readelf="$toolchain/bin/llvm-readelf"
[[ -x "$compiler" && -x "$readelf" ]] || { echo "NDK tools are missing" >&2; exit 1; }
source_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)

mkdir -p -- "$(dirname "$output")"
mkdir -- "$output"
# The C checker must not introduce another vectorized loop. The .S source is
# assembled directly, with no C optimizer, OpenMP runtime or math library.
"$compiler" -std=c11 -O0 -g -Wall -Wextra -Werror \
  -c "$source_dir/vector-sequence.c" -o "$output/checker.o"
"$compiler" -c "$source_dir/vector-sequence.S" -o "$output/sequence.o"
"$compiler" "$output/checker.o" "$output/sequence.o" \
  -Wl,-z,max-page-size=16384 -o "$output/vector-sequence"
"$readelf" -d "$output/vector-sequence" > "$output/dynamic.txt"
while IFS= read -r library; do
  case "$library" in
    libc.so|libdl.so) ;;
    *) echo "Unexpected probe dependency: $library" >&2; exit 1 ;;
  esac
done < <(sed -n 's/.*Shared library: \[\([^]]*\)\].*/\1/p' "$output/dynamic.txt")
printf 'Built ARM64 API 26 instruction diagnostic: %s/vector-sequence\n' "$output"
