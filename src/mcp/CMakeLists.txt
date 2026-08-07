include_directories(${DARKTABLE_BINDIR})

find_package(PkgConfig REQUIRED)
pkg_check_modules(JSON_GLIB REQUIRED json-glib-1.0)

add_executable(darktable-mcp
  main.c
  mcp_jsonrpc.c
  mcp_tools.c
  dt_bridge.c)

set_target_properties(darktable-mcp PROPERTIES LINKER_LANGUAGE C)

target_include_directories(darktable-mcp PRIVATE ${JSON_GLIB_INCLUDE_DIRS})
target_link_directories(darktable-mcp PRIVATE ${JSON_GLIB_LIBRARY_DIRS})
target_link_libraries(darktable-mcp lib_darktable whereami ${JSON_GLIB_LIBRARIES})

if(WIN32)
  _detach_debuginfo(darktable-mcp bin)
else()
  set_target_properties(darktable-mcp
                        PROPERTIES
                        INSTALL_RPATH ${RPATH_ORIGIN}/${REL_BIN_TO_LIBDIR}
                        RUNTIME_OUTPUT_DIRECTORY ${DARKTABLE_BINDIR})
endif(WIN32)

install(TARGETS darktable-mcp DESTINATION ${CMAKE_INSTALL_BINDIR} COMPONENT DTApplication)
