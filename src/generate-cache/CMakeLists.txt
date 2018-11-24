include_directories(${CMAKE_CURRENT_SOURCE_DIR}/..)
include_directories(${CMAKE_CURRENT_BINARY_DIR}/..)
add_executable(darktable-generate-cache main.c)

set_target_properties(darktable-generate-cache PROPERTIES LINKER_LANGUAGE C)
target_link_libraries(darktable-generate-cache lib_darktable)

if (WIN32)
  _detach_debuginfo (darktable-generate-cache bin)
else()
    # Note that $ORIGIN is not a variable but has a special meaning at runtime.
    # The string "$ORIGIN" should end up in the executable as-is.
    set(RPATH_DT "$ORIGIN")
    if (APPLE)
        # The string "@loader_path" should end up in the executable as-is.
        set(RPATH_DT "@loader_path")
    endif()
    set_target_properties(darktable-generate-cache
                          PROPERTIES
                          INSTALL_RPATH ${RPATH_DT}/../${CMAKE_INSTALL_LIBDIR}/darktable)
endif(WIN32)

install(TARGETS darktable-generate-cache DESTINATION ${CMAKE_INSTALL_BINDIR} COMPONENT DTApplication)
