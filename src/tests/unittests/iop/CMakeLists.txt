add_cmocka_mock_test(test_filmicrgb
                     SOURCES test_filmicrgb.c ../util/testimg.c
                     LINK_LIBRARIES lib_darktable cmocka)

add_cmocka_test(test_lut3d_bounds
                SOURCES test_lut3d_bounds.c
                LINK_LIBRARIES lib_darktable cmocka)

add_cmocka_test(test_colorin_hsm
                SOURCES test_colorin_hsm.c
                LINK_LIBRARIES lib_darktable cmocka)

add_cmocka_test(test_rawdenoise
                SOURCES test_rawdenoise.c
                LINK_LIBRARIES lib_darktable cmocka)

add_cmocka_test(test_colorin_rgb
                SOURCES test_colorin_rgb.c
                LINK_LIBRARIES lib_darktable cmocka)

if(USE_OPENCL)
    target_compile_definitions(test_colorin_rgb PRIVATE
        COLORIN_TEST_DATADIR="${CMAKE_BINARY_DIR}/share/darktable"
        COLORIN_TEST_MODULEDIR="${CMAKE_BINARY_DIR}/lib/darktable")
    add_test(NAME test_colorin_rgb_opencl COMMAND test_colorin_rgb --opencl)
    set_tests_properties(test_colorin_rgb_opencl PROPERTIES SKIP_RETURN_CODE 77)
endif()

# Windows: libs have to be copied next to the executable
if(WIN32)
    _copy_required_library(test_filmicrgb lib_darktable)
    _copy_required_library(test_lut3d_bounds lib_darktable)
    _copy_required_library(test_colorin_hsm lib_darktable)
    _copy_required_library(test_colorin_rgb lib_darktable)
    _copy_required_library(test_rawdenoise lib_darktable)
endif(WIN32)
