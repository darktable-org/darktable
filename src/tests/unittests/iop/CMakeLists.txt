add_cmocka_mock_test(test_filmicrgb
                     SOURCES test_filmicrgb.c ../util/testimg.c
                     LINK_LIBRARIES lib_darktable cmocka)

add_cmocka_test(test_lut3d_bounds
                SOURCES test_lut3d_bounds.c
                LINK_LIBRARIES lib_darktable cmocka)

# Windows: libs have to be copied next to the executable
if(WIN32)
    _copy_required_library(test_filmicrgb lib_darktable)
    _copy_required_library(test_lut3d_bounds lib_darktable)
endif(WIN32)
