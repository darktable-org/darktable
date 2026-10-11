# Catalog API tests: darktable started without GUI on an in-memory library,
# as darktable-cli does, with a few integration test images imported. They
# call the src/common/ catalog functions directly, in milliseconds.
set(DT_TEST_IMAGES "${CMAKE_SOURCE_DIR}/src/tests/integration/images")

if(EXISTS "${DT_TEST_IMAGES}/mire1.cr2")
  foreach(suite test_catalog_basics test_catalog_metadata test_catalog_styles test_catalog_tags)
    add_cmocka_test(${suite}
                    SOURCES ${suite}.c catalog_fixture.c
                    LINK_LIBRARIES lib_darktable cmocka)
    target_compile_definitions(${suite} PRIVATE
      DT_TEST_DATADIR="${CMAKE_BINARY_DIR}/share/darktable"
      DT_TEST_MODULEDIR="${CMAKE_BINARY_DIR}/${CMAKE_INSTALL_LIBDIR}/darktable"
      DT_TEST_IMAGES="${DT_TEST_IMAGES}")
    if(WIN32)
      _copy_required_library(${suite} lib_darktable)
    endif(WIN32)
  endforeach()
else()
  message(STATUS "catalog tests disabled: integration test images missing (git submodule)")
endif()
