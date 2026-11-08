# SPDX-License-Identifier: GPL-3.0-or-later
# GoPro's upstream build assumes CMAKE_SOURCE_DIR is its own root. Build only
# its read-only libraries with target-scoped settings instead of importing its
# application targets or overwriting the parent's C/C++ flags.
include(FetchContent)
include_guard(GLOBAL)
if(POLICY CMP0135)
  cmake_policy(SET CMP0135 NEW)
endif()
FetchContent_Declare(dt_gpr_source
  URL https://codeload.github.com/gopro/gpr/tar.gz/446c736a38fb14f51343605c0780d347dc602f89
  URL_HASH SHA256=ddb09f4910f07f43e7d93dbda41d20a6af572be5ac7032052f2fa24b33aeeb0c
  SOURCE_SUBDIR _no_upstream_cmake)
FetchContent_MakeAvailable(dt_gpr_source)
set(_gpr_lib "${dt_gpr_source_SOURCE_DIR}/source/lib")
set(_gpr_sources)
foreach(_component common/private vc5_common vc5_decoder dng_sdk gpr_sdk/private gpr_sdk/public xmp_core expat_lib md5_lib)
  file(GLOB _component_sources CONFIGURE_DEPENDS "${_gpr_lib}/${_component}/*.c" "${_gpr_lib}/${_component}/*.cpp")
  list(APPEND _gpr_sources ${_component_sources})
endforeach()
if(NOT EXISTS "${_gpr_lib}/vc5_decoder/vc5_decoder.c")
  message(FATAL_ERROR "The pinned GoPro SDK must contain its VC-5 RAW decoder")
endif()
add_library(dt_mobile_gpr_sdk STATIC ${_gpr_sources})
set_target_properties(dt_mobile_gpr_sdk PROPERTIES POSITION_INDEPENDENT_CODE ON
  C_VISIBILITY_PRESET hidden CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON)
target_compile_features(dt_mobile_gpr_sdk PRIVATE c_std_99 cxx_std_11)
target_compile_definitions(dt_mobile_gpr_sdk PRIVATE
  GPR_READING=1 GPR_WRITING=0 GPR_JPEG_AVAILABLE=0 GPR_TIMING=0
  XML_STATIC=1 XMP_StaticBuild=1 NDEBUG)
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
  target_compile_definitions(dt_mobile_gpr_sdk PRIVATE qDNG64Bit=1)
endif()
# Old DNG SDK platform inference lacks aarch64; Android ARM64 is little endian
# with 64-bit pointers. Do not let it silently use the legacy 32-bit path.
if(ANDROID)
  target_compile_definitions(dt_mobile_gpr_sdk PRIVATE
    qDNGLittleEndian=1
    XMP_MacBuild=0 XMP_WinBuild=0 XMP_iOSBuild=0 XMP_UNIXBuild=1)
endif()
target_include_directories(dt_mobile_gpr_sdk
  PUBLIC "${_gpr_lib}/gpr_sdk/public" "${_gpr_lib}/common/public"
  PRIVATE "${_gpr_lib}/common/private" "${_gpr_lib}/vc5_common" "${_gpr_lib}/vc5_decoder"
    "${_gpr_lib}/dng_sdk" "${_gpr_lib}/gpr_sdk/private" "${_gpr_lib}/xmp_core/public/include"
    "${_gpr_lib}/md5_lib" "${_gpr_lib}/expat_lib")
target_link_libraries(dt_mobile_gpr_sdk PUBLIC m)

# The SDK bundles old Adobe XMP/Expat symbols also present in Exiv2. Keep the
# decoder in an isolated shared boundary, exporting only our two C entry points.
# A static whole-engine link could bind one library's calls to the other's ABI.
add_library(dt_mobile_gpr SHARED "${CMAKE_CURRENT_LIST_DIR}/../core/dt_mobile_gpr.cpp")
set_target_properties(dt_mobile_gpr PROPERTIES POSITION_INDEPENDENT_CODE ON
  C_VISIBILITY_PRESET hidden CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON)
target_compile_features(dt_mobile_gpr PRIVATE cxx_std_11)
target_compile_definitions(dt_mobile_gpr PRIVATE GPR_READING=1 GPR_WRITING=0 GPR_JPEG_AVAILABLE=0 GPR_TIMING=0)
target_include_directories(dt_mobile_gpr PUBLIC "${CMAKE_CURRENT_LIST_DIR}/../core" "${CMAKE_CURRENT_LIST_DIR}/../include")
find_package(Threads REQUIRED)
target_link_libraries(dt_mobile_gpr PRIVATE dt_mobile_gpr_sdk Threads::Threads)
if(UNIX AND NOT APPLE)
  target_link_options(dt_mobile_gpr PRIVATE "-Wl,-Bsymbolic"
    "-Wl,--version-script=${CMAKE_CURRENT_LIST_DIR}/gpr-exports.map")
  set_property(TARGET dt_mobile_gpr APPEND PROPERTY LINK_DEPENDS "${CMAKE_CURRENT_LIST_DIR}/gpr-exports.map")
endif()
