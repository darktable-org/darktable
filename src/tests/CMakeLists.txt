include_directories("${CMAKE_CURRENT_BINARY_DIR}/../")

add_executable(darktable-test-variables variables.c)
target_link_libraries(darktable-test-variables lib_darktable)

if(WIN32)
    # This tester sets up a darktable instance (of sorts). Hence it expects libraries at ../lib/darktable
    # Easiest way to comply with this on Windows: Put tester executable in same directory as darktable executable
    set_target_properties(darktable-test-variables PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${DARKTABLE_BINDIR}
    )
endif(WIN32)

add_subdirectory(unittests)

add_executable(darktable-test-nikon-he nikon_he.cc)
target_link_libraries(darktable-test-nikon-he PRIVATE darktable_nikon_he_codec)
add_test(NAME imageio_nikon_he COMMAND darktable-test-nikon-he)

add_executable(darktable-test-nikon-z5ii nikon_z5ii.cc)
target_link_libraries(darktable-test-nikon-z5ii PRIVATE darktable_nikon_he Threads::Threads ${Glib_LIBRARIES})
target_compile_definitions(darktable-test-nikon-z5ii PRIVATE
  NIKON_Z5II_CAMERAS_XML="${CMAKE_SOURCE_DIR}/src/external/rawspeed/data/cameras.xml")
add_test(NAME imageio_nikon_z5ii COMMAND darktable-test-nikon-z5ii)
set_tests_properties(imageio_nikon_z5ii PROPERTIES SKIP_RETURN_CODE 77)
