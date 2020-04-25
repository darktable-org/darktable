include_directories(${CMAKE_CURRENT_BINARY_DIR}/..)
add_executable(darktable-generate-cache main.c)

set_target_properties(darktable-generate-cache PROPERTIES LINKER_LANGUAGE C)
target_link_libraries(darktable-generate-cache lib_darktable)

if (WIN32)
  _detach_debuginfo (darktable-generate-cache bin)
else()
    set_target_properties(darktable-generate-cache
                          PROPERTIES
                          INSTALL_RPATH ${CMAKE_INSTALL_LIBDIR_RPATH})
endif(WIN32)

install(TARGETS darktable-generate-cache DESTINATION ${CMAKE_INSTALL_BINDIR} COMPONENT DTApplication)
