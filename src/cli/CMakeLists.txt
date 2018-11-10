include_directories(${CMAKE_CURRENT_SOURCE_DIR}/..)
include_directories(${CMAKE_CURRENT_BINARY_DIR}/..)
add_executable(darktable-cli main.c)

set_target_properties(darktable-cli PROPERTIES LINKER_LANGUAGE C)
target_link_libraries(darktable-cli lib_darktable)

if (WIN32)
  _detach_debuginfo (darktable-cli bin)
else()
    # Note that $ORIGIN is not a variable but has a special meaning at runtime.
    # The string "$ORIGIN" should end up in the executable as-is.
    set(RPATH_DT "$ORIGIN")
    if (APPLE)
        # The string "@loader_path" should end up in the executable as-is.
        set(RPATH_DT "@loader_path")
    endif()
    set_target_properties(darktable-cli
                          PROPERTIES
                          INSTALL_RPATH ${RPATH_DT}/../${CMAKE_INSTALL_LIBDIR}/darktable)
endif(WIN32)

install(TARGETS darktable-cli DESTINATION ${CMAKE_INSTALL_BINDIR} COMPONENT DTApplication)
