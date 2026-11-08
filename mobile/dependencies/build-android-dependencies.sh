#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail

readonly VCPKG_TAG=2025.06.13
readonly ABI=arm64-android
readonly API=26
readonly ANDROID_TARGET=aarch64-linux-android
readonly root_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
readonly triplet_dir="$root_dir/mobile/dependencies/triplets"
readonly source_dir=${DT_MOBILE_VCPKG_ROOT:-"$root_dir/.vcpkg/$VCPKG_TAG"}
readonly install_dir=${DT_MOBILE_VCPKG_INSTALLED:-"$root_dir/.vcpkg/installed"}
readonly manifest_dir=${DT_MOBILE_VCPKG_MANIFEST_ROOT:-"$root_dir/mobile/dependencies"}
readonly overlay_ports=${DT_MOBILE_VCPKG_OVERLAY_PORTS:-}
readonly environment_file=${DT_MOBILE_VCPKG_ENV_FILE:-"$root_dir/mobile/dependencies/android-dependencies.env"}
readonly cmake_toolchain_file="$source_dir/scripts/buildsystems/vcpkg.cmake"

: "${ANDROID_NDK_HOME:?ANDROID_NDK_HOME must name NDK 27.2.12479018}"
if [[ $(basename "$ANDROID_NDK_HOME") != 27.2.12479018 ]]; then
  echo "Expected Android NDK 27.2.12479018, got $ANDROID_NDK_HOME" >&2
  exit 1
fi
if [[ ! -d "$source_dir/.git" ]]; then
  mkdir -p "$(dirname "$source_dir")"
  git clone --branch "$VCPKG_TAG" --depth 1 https://github.com/microsoft/vcpkg.git "$source_dir"
fi
test "$(git -C "$source_dir" describe --tags --exact-match)" = "$VCPKG_TAG"
[[ -x "$source_dir/vcpkg" ]] || "$source_dir/bootstrap-vcpkg.sh" -disableMetrics

export ANDROID_NDK_HOME
export ANDROID_NDK_ROOT="$ANDROID_NDK_HOME"
export ANDROID_ABI=arm64-v8a
export ANDROID_PLATFORM=android-$API
export CMAKE_ANDROID_ARCH_ABI=arm64-v8a
export CMAKE_ANDROID_ARCH=aarch64
export CMAKE_TOOLCHAIN_FILE="$cmake_toolchain_file"
export VCPKG_ROOT="$source_dir"
export VCPKG_DEFAULT_TRIPLET=$ABI
export VCPKG_TARGET_TRIPLET=$ABI
export VCPKG_FORCE_SYSTEM_BINARIES=1

# Fail before a lengthy dependency build if any caller reintroduces a 32-bit
# Android target through the environment.
if armv7_environment=$(env | grep -Ei '(^|=)(armeabi-v7a|armv7|[^[:alnum:]]mthumb|march=armv7-a)'); then
  echo "An ARMv7 Android setting is present in the dependency build environment" >&2
  printf '%s\n' "$armv7_environment" >&2
  exit 1
fi
[[ $ANDROID_ABI == arm64-v8a && $ANDROID_PLATFORM == android-26 ]]
[[ $ANDROID_TARGET == aarch64-linux-android ]]
test -f "$ANDROID_NDK_HOME/build/cmake/android.toolchain.cmake"
test -f "$CMAKE_TOOLCHAIN_FILE"

# Validate the checked-in triplet itself, rather than trusting its filename.
cmake -DTRIPLET_FILE="$triplet_dir/$ABI.cmake" \
  -P "$root_dir/mobile/dependencies/validate-android-triplet.cmake"

echo 'Android dependency build environment:'
for name in ANDROID_NDK_HOME ANDROID_NDK_ROOT VCPKG_ROOT \
  VCPKG_DEFAULT_TRIPLET VCPKG_TARGET_TRIPLET CMAKE_TOOLCHAIN_FILE; do
  printf '%s=%s\n' "$name" "${!name-<unset>}"
done
env | LC_ALL=C sort | grep -E '^(ANDROID_(ABI|PLATFORM)|CMAKE_ANDROID_)' || true
printf 'CPPFLAGS=%s\n' "${CPPFLAGS-<unset>}"
printf 'CMAKE_ANDROID_ARCH_ABI=%s\n' "$CMAKE_ANDROID_ARCH_ABI"

install_arguments=(
  --triplet "$ABI"
  --overlay-triplets="$triplet_dir"
  --x-manifest-root="$manifest_dir"
  --x-install-root="$install_dir"
)
if [[ ${DT_MOBILE_VCPKG_CLEAN_AFTER_BUILD:-1} == 1 ]]; then
  install_arguments+=(--clean-after-build)
fi
if [[ -n "$overlay_ports" ]]; then
  install_arguments+=("--overlay-ports=$overlay_ports")
fi
"$source_dir/vcpkg" install "${install_arguments[@]}" "$@"

# A resolution-only probe has not installed libraries; do not emit a build
# environment file that a later caller could mistake for a completed bundle.
for argument in "$@"; do
  [[ "$argument" != --dry-run ]] || exit 0
done

printf 'VCPKG_ROOT=%s\nVCPKG_INSTALLED_DIR=%s\nVCPKG_DEFAULT_TRIPLET=%s\nVCPKG_TARGET_TRIPLET=%s\nANDROID_ABI=%s\nANDROID_PLATFORM=%s\n' \
  "$source_dir" "$install_dir" "$ABI" "$ABI" "$ANDROID_ABI" "$ANDROID_PLATFORM" \
  > "$environment_file"
