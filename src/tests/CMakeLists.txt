include_directories(${CMAKE_CURRENT_SOURCE_DIR}/..)
include_directories(${CMAKE_CURRENT_BINARY_DIR}/..)

add_executable(darktable-test-variables variables.c)

set_target_properties(darktable-test-variables PROPERTIES INSTALL_RPATH "$ORIGIN/../")
set_target_properties(darktable-test-variables PROPERTIES LINKER_LANGUAGE C)
target_link_libraries(darktable-test-variables lib_darktable)

