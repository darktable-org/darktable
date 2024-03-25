include_directories(${DARKTABLE_BINDIR})

if(WIN32)
  set(CLI_RESOURCE_OBJECT "${CMAKE_CURRENT_BINARY_DIR}/../win/cli.rc")
endif(WIN32)
add_executable(darktable-generate-cache main.c ${CLI_RESOURCE_OBJECT})

set_target_properties(darktable-generate-cache PROPERTIES LINKER_LANGUAGE C)
target_link_libraries(darktable-generate-cache lib_darktable whereami)

if (WIN32)
  _detach_debuginfo (darktable-generate-cache bin)
else()
    set_target_properties(darktable-generate-cache
                          PROPERTIES
                          INSTALL_RPATH ${RPATH_ORIGIN}/${REL_BIN_TO_LIBDIR}
                          RUNTIME_OUTPUT_DIRECTORY ${DARKTABLE_BINDIR})
endif(WIN32)

install(TARGETS darktable-generate-cache DESTINATION ${CMAKE_INSTALL_BINDIR} COMPONENT DTApplication)
