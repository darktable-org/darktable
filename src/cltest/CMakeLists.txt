include_directories(${DARKTABLE_BINDIR})
add_executable(darktable-cltest main.c)

set_target_properties(darktable-cltest PROPERTIES LINKER_LANGUAGE C)
target_link_libraries(darktable-cltest lib_darktable)

if (WIN32)
  _detach_debuginfo (darktable-cltest bin)
else()
    # Note that $ORIGIN is not a variable but has a special meaning at runtime.
    # The string "$ORIGIN" should end up in the executable as-is.
    set(RPATH_DT "$ORIGIN")
    if (APPLE)
        # The string "@loader_path" should end up in the executable as-is.
        set(RPATH_DT "@loader_path")
    endif()
    set_target_properties(darktable-cltest
                          PROPERTIES
                          INSTALL_RPATH ${CMAKE_INSTALL_LIBDIR_RPATH}
                          RUNTIME_OUTPUT_DIRECTORY ${DARKTABLE_BINDIR})
endif(WIN32)

install(TARGETS darktable-cltest DESTINATION ${CMAKE_INSTALL_BINDIR} COMPONENT DTApplication)
