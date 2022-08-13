include_directories(${DARKTABLE_BINDIR})
add_executable(darktable-cli main.c)

set_target_properties(darktable-cli PROPERTIES LINKER_LANGUAGE C)
target_link_libraries(darktable-cli lib_darktable whereami)

if (WIN32)
  _detach_debuginfo (darktable-cli bin)
else()
    set_target_properties(darktable-cli
                          PROPERTIES
                          INSTALL_RPATH ${RPATH_ORIGIN}/${REL_BIN_TO_LIBDIR}
                          RUNTIME_OUTPUT_DIRECTORY ${DARKTABLE_BINDIR})
endif(WIN32)

install(TARGETS darktable-cli DESTINATION ${CMAKE_INSTALL_BINDIR} COMPONENT DTApplication)
