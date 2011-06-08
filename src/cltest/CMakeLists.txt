
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/..)
include_directories(${CMAKE_CURRENT_BINARY_DIR}/..)
add_executable(darktable-cltest main.c)

set_target_properties(darktable-cltest PROPERTIES CMAKE_BUILD_WITH_INSTALL_RPATH TRUE)
set_target_properties(darktable-cltest PROPERTIES CMAKE_INSTALL_RPATH_USE_LINK_PATH FALSE)
set_target_properties(darktable-cltest PROPERTIES INSTALL_RPATH $ORIGIN/../${LIB_INSTALL}/darktable)
set_target_properties(darktable-cltest PROPERTIES LINKER_LANGUAGE C)
target_link_libraries(darktable-cltest lib_darktable)
install(TARGETS darktable-cltest DESTINATION bin)
