include_directories("${CMAKE_CURRENT_BINARY_DIR}/../")

add_executable(darktable-test-variables variables.c)
target_link_libraries(darktable-test-variables lib_darktable)

add_executable(darktable-test-display-transport
               display_transport.c ../common/display_transport.c)
target_include_directories(darktable-test-display-transport PRIVATE ${LCMS2_INCLUDE_DIRS})
target_link_libraries(darktable-test-display-transport PRIVATE ${LCMS2_LIBRARIES})
if(NOT WIN32)
  target_link_libraries(darktable-test-display-transport PRIVATE m)
endif()
add_test(NAME display-transport COMMAND darktable-test-display-transport)

if(WIN32)
    # This tester sets up a darktable instance (of sorts). Hence it expects libraries at ../lib/darktable
    # Easiest way to comply with this on Windows: Put tester executable in same directory as darktable executable
    set_target_properties(darktable-test-variables PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${DARKTABLE_BINDIR}
    )
endif(WIN32)

add_subdirectory(unittests)
