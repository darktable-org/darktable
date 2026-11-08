cmake_minimum_required(VERSION 3.22.1)
project(dt_sidecar_tests LANGUAGES C)

enable_testing()
find_package(PkgConfig REQUIRED)
pkg_check_modules(SIDECAR_GLIB REQUIRED IMPORTED_TARGET glib-2.0)

get_filename_component(_source_dir "${CMAKE_CURRENT_LIST_DIR}/../../src" ABSOLUTE)
add_executable(dt_sidecar_test sidecar_test.c "${_source_dir}/common/sidecar.c")
target_compile_features(dt_sidecar_test PRIVATE c_std_99)
target_compile_options(dt_sidecar_test PRIVATE -Wall -Wextra -Werror)
target_include_directories(dt_sidecar_test PRIVATE "${_source_dir}")
target_link_libraries(dt_sidecar_test PRIVATE PkgConfig::SIDECAR_GLIB)
add_test(NAME dt_sidecar_test COMMAND dt_sidecar_test)
