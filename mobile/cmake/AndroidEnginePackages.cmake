# SPDX-License-Identifier: GPL-3.0-or-later
# Included before src/ so every pkg-config lookup sees target metadata only.
if(NOT ANDROID OR NOT BUILD_MOBILE_LIBRARY_ONLY)
  message(FATAL_ERROR "AndroidEnginePackages is for the Android library-only engine build")
endif()
# The NDK's default legacy toolchain deliberately sets CMAKE_SYSTEM_VERSION=1
# to disable CMake's separate NDK handling. Check the resolved Android API,
# rather than confusing that sentinel with the compiler's target API.
set(_dt_engine_android_api "${CMAKE_SYSTEM_VERSION}")
if(DEFINED ANDROID_PLATFORM_LEVEL)
  set(_dt_engine_android_api "${ANDROID_PLATFORM_LEVEL}")
elseif(DEFINED CMAKE_ANDROID_API)
  set(_dt_engine_android_api "${CMAKE_ANDROID_API}")
elseif(DEFINED ANDROID_NATIVE_API_LEVEL)
  set(_dt_engine_android_api "${ANDROID_NATIVE_API_LEVEL}")
endif()
if(NOT ANDROID_ABI STREQUAL "arm64-v8a" OR NOT _dt_engine_android_api STREQUAL "26"
   OR NOT VCPKG_TARGET_TRIPLET STREQUAL "arm64-android")
  message(FATAL_ERROR "The engine bootstrap requires the pinned Android ARM64/API 26 triplet")
endif()
if(NOT VCPKG_INSTALLED_DIR)
  message(FATAL_ERROR "VCPKG_INSTALLED_DIR must name the installed Android engine dependencies")
endif()
cmake_path(ABSOLUTE_PATH VCPKG_INSTALLED_DIR NORMALIZE OUTPUT_VARIABLE _dt_engine_installed)
set(_dt_engine_prefix "${_dt_engine_installed}/${VCPKG_TARGET_TRIPLET}")
set(_dt_engine_pc "${_dt_engine_prefix}/lib/pkgconfig:${_dt_engine_prefix}/share/pkgconfig")
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
  set(_dt_engine_pc "${_dt_engine_prefix}/debug/lib/pkgconfig:${_dt_engine_pc}")
endif()
set(ENV{PKG_CONFIG_PATH} "")
set(ENV{PKG_CONFIG_LIBDIR} "${_dt_engine_pc}")
set(ENV{PKG_CONFIG_SYSROOT_DIR} "")
set(PKG_CONFIG_USE_CMAKE_PREFIX_PATH FALSE)
# Several desktop Find modules expose just one archive. Retain the complete
# private dependency chain when linking the Android static/PIC bundle.
set(PKG_CONFIG_ARGN --static)
find_package(PkgConfig REQUIRED)
pkg_check_modules(DT_ANDROID_ENGINE REQUIRED IMPORTED_TARGET GLOBAL
  gtk+-3.0 gio-2.0 gmodule-2.0 gthread-2.0 pangocairo librsvg-2.0
  json-glib-1.0 lensfun lcms2 exiv2 libtiff-4 libpng libxml-2.0)
foreach(_dt_engine_path IN LISTS DT_ANDROID_ENGINE_INCLUDE_DIRS DT_ANDROID_ENGINE_LIBRARY_DIRS)
  cmake_path(IS_PREFIX _dt_engine_prefix "${_dt_engine_path}" NORMALIZE _dt_engine_is_target)
  if(NOT _dt_engine_is_target)
    message(FATAL_ERROR "Android engine pkg-config resolved outside its target prefix: ${_dt_engine_path}")
  endif()
endforeach()
# Legacy desktop Find modules also append bare -l names. Supply only the
# validated target directories for those entries; imported targets above keep
# the full static dependency order and compiler requirements.
link_directories(${DT_ANDROID_ENGINE_LIBRARY_DIRS})
