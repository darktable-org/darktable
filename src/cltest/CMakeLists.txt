include_directories(${DARKTABLE_BINDIR})

if(WIN32)
  set(CLI_RESOURCE_OBJECT "${CMAKE_CURRENT_BINARY_DIR}/../win/cli.rc")
endif(WIN32)
add_executable(darktable-cltest main.c ${CLI_RESOURCE_OBJECT})

set_target_properties(darktable-cltest PROPERTIES LINKER_LANGUAGE C)
target_link_libraries(darktable-cltest lib_darktable)

if (WIN32)
  _detach_debuginfo (darktable-cltest bin)
else()
    set_target_properties(darktable-cltest
                          PROPERTIES
                          INSTALL_RPATH ${RPATH_ORIGIN}/${REL_BIN_TO_LIBDIR}
                          RUNTIME_OUTPUT_DIRECTORY ${DARKTABLE_BINDIR})
endif(WIN32)

install(TARGETS darktable-cltest DESTINATION ${CMAKE_INSTALL_BINDIR} COMPONENT DTApplication)
