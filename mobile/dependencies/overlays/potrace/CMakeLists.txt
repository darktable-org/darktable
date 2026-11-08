# SPDX-License-Identifier: GPL-3.0-or-later
cmake_minimum_required(VERSION 3.22.1)
project(potrace VERSION 1.16 LANGUAGES C)
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
include(CheckIncludeFile)
include(CheckCSourceCompiles)

set(POTRACE_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}" CACHE PATH "Upstream Potrace 1.16 release directory")
if(NOT EXISTS "${POTRACE_SOURCE_DIR}/src/potracelib.c")
  message(FATAL_ERROR "POTRACE_SOURCE_DIR must contain the upstream Potrace 1.16 release")
endif()
check_include_file(inttypes.h HAVE_INTTYPES_H)
if(NOT HAVE_INTTYPES_H)
  message(FATAL_ERROR "Potrace requires inttypes.h and uint64_t")
endif()
check_c_source_compiles("#include <inttypes.h>\nint main(void) { uint64_t x = 0; return (int)x; }" HAVE_UINT64_T)
if(NOT HAVE_UINT64_T)
  message(FATAL_ERROR "Potrace requires uint64_t")
endif()
# Match configure.ac's conservative GCC-compatible compiler workaround.
check_c_source_compiles("#ifndef __GNUC__\n#error not GCC-compatible\n#endif\nint main(void) { return 0; }" HAVE_GCC_LOOP_BUG)
configure_file(config.h.in config.h)

# Exactly the libsources in upstream src/Makefile.am. Compression, rendering,
# file backends and getopt belong to the CLI and are not library dependencies.
add_library(potrace STATIC
  "${POTRACE_SOURCE_DIR}/src/curve.c"
  "${POTRACE_SOURCE_DIR}/src/trace.c"
  "${POTRACE_SOURCE_DIR}/src/decompose.c"
  "${POTRACE_SOURCE_DIR}/src/potracelib.c"
)
add_library(potrace::potrace ALIAS potrace)
target_compile_features(potrace PRIVATE c_std_99)
set_target_properties(potrace PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_compile_definitions(potrace PRIVATE HAVE_CONFIG_H=1)
target_include_directories(potrace PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
target_include_directories(potrace PUBLIC
  "$<BUILD_INTERFACE:${POTRACE_SOURCE_DIR}/src>"
  "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
)
if(NOT WIN32)
  target_link_libraries(potrace PUBLIC m)
endif()

install(TARGETS potrace EXPORT potraceTargets ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}")
install(FILES "${POTRACE_SOURCE_DIR}/src/potracelib.h" DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
install(EXPORT potraceTargets NAMESPACE potrace:: DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/potrace")
configure_package_config_file(potraceConfig.cmake.in "${CMAKE_CURRENT_BINARY_DIR}/potraceConfig.cmake"
  INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/potrace")
write_basic_package_version_file("${CMAKE_CURRENT_BINARY_DIR}/potraceConfigVersion.cmake"
  VERSION "${PROJECT_VERSION}" COMPATIBILITY SameMajorVersion)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/potraceConfig.cmake"
  "${CMAKE_CURRENT_BINARY_DIR}/potraceConfigVersion.cmake" DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/potrace")
configure_file(potrace.pc.in potrace.pc @ONLY)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/potrace.pc" DESTINATION "${CMAKE_INSTALL_LIBDIR}/pkgconfig")

option(POTRACE_BUILD_PROBE "Build a trace fixture and native-link verification target" OFF)
if(POTRACE_BUILD_PROBE)
  if(ANDROID)
    add_library(potrace_link_probe SHARED "${CMAKE_CURRENT_SOURCE_DIR}/trace_probe.c")
    target_compile_definitions(potrace_link_probe PRIVATE POTRACE_ANDROID_LINK_PROBE=1)
    target_link_options(potrace_link_probe PRIVATE -Wl,--no-undefined)
    target_link_libraries(potrace_link_probe PRIVATE
      "-Wl,--whole-archive,$<TARGET_FILE:potrace>,--no-whole-archive" potrace)
  else()
    enable_testing()
    add_executable(potrace_trace_test trace_probe.c)
    target_link_libraries(potrace_trace_test PRIVATE potrace)
    target_compile_options(potrace_trace_test PRIVATE -Wall -Wextra -Werror)
    add_test(NAME potrace_trace_test COMMAND potrace_trace_test)
  endif()
endif()
