#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
readonly root_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
readonly source_dir=${DT_MOBILE_VCPKG_ROOT:-"$root_dir/.vcpkg/2025.06.13"}
readonly build_dir=${DT_MOBILE_ENGINE_BUILD:-"$root_dir/.vcpkg/engine/build"}
readonly install_dir=${DT_MOBILE_VCPKG_INSTALLED:-"$root_dir/.vcpkg/engine/installed"}
readonly build_type=${DT_MOBILE_ENGINE_BUILD_TYPE:-Release}
: "${ANDROID_NDK_HOME:?ANDROID_NDK_HOME must name NDK 27.2.12479018}"
case "$build_type" in Debug|Release|RelWithDebInfo) ;; *) echo 'Unsupported engine build type' >&2; exit 1 ;; esac
resolution_only=false
for argument in "$@"; do
  [[ "$argument" != --dry-run ]] || resolution_only=true
done
if ! "$resolution_only" && [[ ! -f "$root_dir/src/external/LibRaw/libraw/libraw_version.h" ]]; then
  echo 'Initialize the source submodules with git submodule update --init --recursive before building the engine.' >&2
  exit 1
fi
mkdir -p "$build_dir"
export DT_MOBILE_VCPKG_MANIFEST_ROOT="$root_dir/mobile/dependencies/engine"
export DT_MOBILE_VCPKG_OVERLAY_PORTS="$root_dir/mobile/dependencies/overlays"
export DT_MOBILE_VCPKG_INSTALLED="$install_dir"
export DT_MOBILE_VCPKG_ENV_FILE="$build_dir/android-dependencies.env"
# Retain port logs while bringing up the complete editor dependency graph.
export DT_MOBILE_VCPKG_CLEAN_AFTER_BUILD=${DT_MOBILE_VCPKG_CLEAN_AFTER_BUILD:-0}
"$root_dir/mobile/dependencies/build-android-dependencies.sh" "$@"
if "$resolution_only"; then exit 0; fi
cmake -S "$root_dir" -B "$build_dir" -G Ninja \
  -DCMAKE_BUILD_TYPE="$build_type" \
  -DCMAKE_TOOLCHAIN_FILE="$source_dir/scripts/buildsystems/vcpkg.cmake" \
  -DVCPKG_CHAINLOAD_TOOLCHAIN_FILE="$ANDROID_NDK_HOME/build/cmake/android.toolchain.cmake" \
  -DVCPKG_TARGET_TRIPLET=arm64-android \
  -DVCPKG_OVERLAY_TRIPLETS="$root_dir/mobile/dependencies/triplets" \
  -DVCPKG_INSTALLED_DIR="$install_dir" -DVCPKG_MANIFEST_MODE=OFF \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-26 -DANDROID_STL=c++_shared \
  -DANDROID_SUPPORT_FLEXIBLE_PAGE_SIZES=ON \
  -DBUILD_MOBILE_LIBRARY_ONLY=ON -DDT_MOBILE_ANDROID_DEPENDENCIES=OFF \
  -DDT_MOBILE_WITH_DARKTABLE_ENGINE=ON \
  -DDT_MOBILE_WITH_GPR_SDK=OFF -DDT_MOBILE_BUILD_TESTS=OFF -DBUILD_TESTING=OFF \
  -DUSE_LIBRAW=ON -DDONT_USE_INTERNAL_LIBRAW=OFF -DUSE_ISOBMFF=ON \
  -DUSE_LUA=OFF -DUSE_MAP=OFF -DBUILD_PRINT=OFF -DUSE_COLORD=OFF \
  -DUSE_KWALLET=OFF -DUSE_LIBSECRET=OFF -DUSE_GRAPHICSMAGICK=OFF \
  -DUSE_IMAGEMAGICK=OFF -DUSE_GMIC=OFF -DUSE_SDL2=OFF -DUSE_OPENCL=OFF \
  -DUSE_AI=OFF -DUSE_JXL=OFF -DUSE_AVIF=OFF -DUSE_HEIF=OFF -DUSE_OPENJPEG=OFF \
  -DUSE_WEBP=OFF -DUSE_OPENEXR=OFF -DUSE_XCF=ON -DUSE_ICU=OFF \
  -DUSE_CAMERA_SUPPORT=OFF -DUSE_PORTMIDI=OFF -DUSE_XMLLINT=OFF
cmake --build "$build_dir" --target dt_mobile_runtime --parallel "${CMAKE_BUILD_PARALLEL_LEVEL:-2}"
printf 'Built the experimental native engine graph in %s\n' "$build_dir"
printf 'APK resource/module packaging and Android execution still require verification.\n'
