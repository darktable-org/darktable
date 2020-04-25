include_directories(${CMAKE_CURRENT_BINARY_DIR}/..)
add_executable(darktable-cli main.c)

set_target_properties(darktable-cli PROPERTIES LINKER_LANGUAGE C)
target_link_libraries(darktable-cli lib_darktable)

if (WIN32)
  _detach_debuginfo (darktable-cli bin)
else()
    set_target_properties(darktable-cli
                          PROPERTIES
                          INSTALL_RPATH ${CMAKE_INSTALL_LIBDIR_RPATH})
endif(WIN32)

install(TARGETS darktable-cli DESTINATION ${CMAKE_INSTALL_BINDIR} COMPONENT DTApplication)
