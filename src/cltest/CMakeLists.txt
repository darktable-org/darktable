cmake_minimum_required(VERSION 2.8.5)

include_directories(${CMAKE_CURRENT_SOURCE_DIR}/..)
include_directories(${CMAKE_CURRENT_BINARY_DIR}/..)
add_executable(darktable-cltest main.c)

set_target_properties(darktable-cltest PROPERTIES CMAKE_BUILD_WITH_INSTALL_RPATH TRUE)
set_target_properties(darktable-cltest PROPERTIES CMAKE_INSTALL_RPATH_USE_LINK_PATH FALSE)
set_target_properties(darktable-cltest PROPERTIES INSTALL_RPATH ${RPATH_DT}/../${CMAKE_INSTALL_LIBDIR}/darktable)
set_target_properties(darktable-cltest PROPERTIES LINKER_LANGUAGE C)
target_link_libraries(darktable-cltest lib_darktable)
install(TARGETS darktable-cltest DESTINATION ${CMAKE_INSTALL_BINDIR})
