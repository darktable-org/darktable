#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
readonly root_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
readonly source_dir=${DT_MOBILE_VCPKG_ROOT:-"$root_dir/.vcpkg/2025.06.13"}
readonly probe_dir=${DT_MOBILE_GTK_PROBE_BUILD:-"$root_dir/.vcpkg/gtk-probe/build"}
readonly install_dir=${DT_MOBILE_VCPKG_INSTALLED:-"$root_dir/.vcpkg/gtk-probe/installed"}
: "${ANDROID_NDK_HOME:?ANDROID_NDK_HOME must name NDK 27.2.12479018}"
mkdir -p "$probe_dir"
export DT_MOBILE_VCPKG_MANIFEST_ROOT="$root_dir/mobile/dependencies/gtk-probe"
export DT_MOBILE_VCPKG_OVERLAY_PORTS="$root_dir/mobile/dependencies/overlays"
export DT_MOBILE_VCPKG_INSTALLED="$install_dir"
export DT_MOBILE_VCPKG_ENV_FILE="$probe_dir/android-dependencies.env"
# Retain logs and checksum-verified mirror archives while bringing up this
# optional route. The normal dependency driver's cleanup default is unchanged.
export DT_MOBILE_VCPKG_CLEAN_AFTER_BUILD=${DT_MOBILE_VCPKG_CLEAN_AFTER_BUILD:-0}
"$root_dir/mobile/dependencies/build-android-dependencies.sh" "$@"
for argument in "$@"; do
  [[ "$argument" != --dry-run ]] || exit 0
done
cmake -S "$root_dir/mobile/dependencies/gtk-probe" -B "$probe_dir" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE="$source_dir/scripts/buildsystems/vcpkg.cmake" \
  -DVCPKG_CHAINLOAD_TOOLCHAIN_FILE="$ANDROID_NDK_HOME/build/cmake/android.toolchain.cmake" \
  -DVCPKG_TARGET_TRIPLET=arm64-android \
  -DVCPKG_OVERLAY_TRIPLETS="$root_dir/mobile/dependencies/triplets" \
  -DVCPKG_INSTALLED_DIR="$install_dir" -DVCPKG_MANIFEST_MODE=OFF \
  -DDT_MOBILE_GTK_PREFIX="$install_dir/arm64-android" \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-26 -DANDROID_STL=c++_shared
cmake --build "$probe_dir" --target dt_mobile_gtk_probe --parallel "${CMAKE_BUILD_PARALLEL_LEVEL:-2}"
for candidate in "$ANDROID_NDK_HOME"/toolchains/llvm/prebuilt/*/bin/llvm-readelf; do
  if [[ -x "$candidate" ]]; then
    "$candidate" -h -d --dyn-syms --wide "$probe_dir/libdt_mobile_gtk_probe.so" > "$probe_dir/elf-report.txt"
    grep -q 'Machine:.*AArch64' "$probe_dir/elf-report.txt"
    if grep -E 'UND.* (shm_open|shm_unlink|XOpenDisplay|wl_display_connect)(@|$)' "$probe_dir/elf-report.txt"; then
      echo 'GTK probe still references an unavailable desktop transport' >&2
      exit 1
    fi
    while IFS= read -r soname; do
      case "$soname" in
        libc++_shared.so|libc.so|libm.so|libdl.so|liblog.so|libandroid.so|libEGL.so|libGLESv2.so|libz.so) ;;
        *) printf 'GTK probe has an unexpected runtime library: %s\n' "$soname" >&2; exit 1 ;;
      esac
    done < <(sed -n 's/.*(NEEDED).*\[\([^]]*\)\].*/\1/p' "$probe_dir/elf-report.txt")
    printf 'Android ARM64 GTK link probe built: %s\n' "$probe_dir/libdt_mobile_gtk_probe.so"
    printf 'This verifies linking only; run the entry point on Android before claiming runtime support.\n'
    exit 0
  fi
done
echo 'Unable to find the pinned NDK llvm-readelf' >&2
exit 1
