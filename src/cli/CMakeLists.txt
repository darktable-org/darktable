find_package(PkgConfig REQUIRED)
pkg_check_modules(DARKTABLE_LIVE_BRIDGE_DEPS REQUIRED glib-2.0 gio-2.0 json-glib-1.0)

include_directories(${DARKTABLE_BINDIR})
add_executable(darktable-cli main.c)
add_executable(darktable-wb-resolve temperature_resolve.c)
add_executable(darktable-live-bridge darktable_live_bridge.c)

set_target_properties(darktable-cli PROPERTIES LINKER_LANGUAGE C)
set_target_properties(darktable-wb-resolve PROPERTIES LINKER_LANGUAGE C)
set_target_properties(darktable-live-bridge PROPERTIES LINKER_LANGUAGE C)
target_link_libraries(darktable-cli lib_darktable whereami)
target_link_libraries(darktable-wb-resolve lib_darktable whereami)
target_include_directories(darktable-live-bridge PRIVATE ${DARKTABLE_LIVE_BRIDGE_DEPS_INCLUDE_DIRS})
target_link_directories(darktable-live-bridge PRIVATE ${DARKTABLE_LIVE_BRIDGE_DEPS_LIBRARY_DIRS})
target_link_libraries(darktable-live-bridge PRIVATE ${DARKTABLE_LIVE_BRIDGE_DEPS_LIBRARIES})

if (WIN32)
  _detach_debuginfo (darktable-cli bin)
  _detach_debuginfo (darktable-wb-resolve bin)
  _detach_debuginfo (darktable-live-bridge bin)
else()
    set_target_properties(darktable-cli
                          PROPERTIES
                          INSTALL_RPATH ${RPATH_ORIGIN}/${REL_BIN_TO_LIBDIR}
                          RUNTIME_OUTPUT_DIRECTORY ${DARKTABLE_BINDIR})
    set_target_properties(darktable-wb-resolve
                          PROPERTIES
                          INSTALL_RPATH ${RPATH_ORIGIN}/${REL_BIN_TO_LIBDIR}
                          RUNTIME_OUTPUT_DIRECTORY ${DARKTABLE_BINDIR})
    set_target_properties(darktable-live-bridge
                          PROPERTIES
                          INSTALL_RPATH ${RPATH_ORIGIN}/${REL_BIN_TO_LIBDIR}
                          RUNTIME_OUTPUT_DIRECTORY ${DARKTABLE_BINDIR})
endif(WIN32)

install(TARGETS darktable-cli DESTINATION ${CMAKE_INSTALL_BINDIR} COMPONENT DTApplication)
install(TARGETS darktable-wb-resolve DESTINATION ${CMAKE_INSTALL_BINDIR} COMPONENT DTApplication)
install(TARGETS darktable-live-bridge DESTINATION ${CMAKE_INSTALL_BINDIR} COMPONENT DTApplication)
