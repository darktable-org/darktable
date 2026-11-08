# SPDX-License-Identifier: GPL-3.0-or-later
cmake_minimum_required(VERSION 3.22.1)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(dt_introspection_tests LANGUAGES C)
endif()

enable_testing()
find_package(Perl REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(INTROSPECTION_TEST_GLIB REQUIRED IMPORTED_TARGET glib-2.0)

get_filename_component(_introspection_dir "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
get_filename_component(_source_dir "${_introspection_dir}/../../src" ABSOLUTE)
set(_fixture "${CMAKE_CURRENT_LIST_DIR}/fixtures/enum_params.c")
set(_generated "${CMAKE_CURRENT_BINARY_DIR}/introspection_enum_params.c")
add_custom_command(OUTPUT "${_generated}"
  COMMAND "${PERL_EXECUTABLE}" "${_introspection_dir}/parser.pl"
    "${_source_dir}" "${_fixture}" "${_generated}"
  DEPENDS "${_fixture}" "${_source_dir}/common/introspection.h"
    "${_introspection_dir}/parser.pl" "${_introspection_dir}/scanner.pm"
    "${_introspection_dir}/parser.pm" "${_introspection_dir}/ast.pm"
    "${_introspection_dir}/code_gen.pm"
  VERBATIM)

add_executable(dt_introspection_enum_test "${_generated}" enum_metadata_test.c)
target_compile_features(dt_introspection_enum_test PRIVATE c_std_99)
target_include_directories(dt_introspection_enum_test PRIVATE "${_source_dir}")
target_link_libraries(dt_introspection_enum_test PRIVATE PkgConfig::INTROSPECTION_TEST_GLIB)
add_test(NAME dt_introspection_enum_test COMMAND dt_introspection_enum_test)

set(_double_fixture "${CMAKE_CURRENT_LIST_DIR}/fixtures/double_params.c")
set(_double_generated "${CMAKE_CURRENT_BINARY_DIR}/introspection_double_params.c")
add_custom_command(OUTPUT "${_double_generated}"
  COMMAND "${PERL_EXECUTABLE}" "${_introspection_dir}/parser.pl"
    "${_source_dir}" "${_double_fixture}" "${_double_generated}"
  DEPENDS "${_double_fixture}" "${_source_dir}/common/introspection.h"
    "${_introspection_dir}/parser.pl" "${_introspection_dir}/scanner.pm"
    "${_introspection_dir}/parser.pm" "${_introspection_dir}/ast.pm"
    "${_introspection_dir}/code_gen.pm"
  VERBATIM)
add_executable(dt_introspection_double_test "${_double_generated}" double_metadata_test.c)
target_compile_features(dt_introspection_double_test PRIVATE c_std_99)
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(dt_introspection_double_test PRIVATE -Wall -Wextra -Werror)
endif()
target_include_directories(dt_introspection_double_test PRIVATE "${_source_dir}")
target_link_libraries(dt_introspection_double_test PRIVATE PkgConfig::INTROSPECTION_TEST_GLIB)
add_test(NAME dt_introspection_double_test COMMAND dt_introspection_double_test)
