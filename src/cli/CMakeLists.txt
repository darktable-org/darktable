include_directories(${CMAKE_CURRENT_SOURCE_DIR}/..)
include_directories(${CMAKE_CURRENT_BINARY_DIR}/..)
add_executable(darktable-cli main.c)

set_target_properties(darktable-cli PROPERTIES LINKER_LANGUAGE C)
target_link_libraries(darktable-cli lib_darktable)
install(TARGETS darktable-cli DESTINATION ${CMAKE_INSTALL_BINDIR})
