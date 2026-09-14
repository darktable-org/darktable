add_cmocka_mock_test(test_filmicrgb
                     SOURCES test_filmicrgb.c ../util/testimg.c
                     LINK_LIBRARIES lib_darktable cmocka
                     MOCKS dt_iop_color_picker_reset)

add_cmocka_test(test_zonesystem_bounds
                SOURCES test_zonesystem_bounds.c
                LINK_LIBRARIES lib_darktable cmocka)

add_cmocka_test(test_colormapping_bounds
                SOURCES test_colormapping_bounds.c
                LINK_LIBRARIES lib_darktable cmocka)

add_cmocka_test(test_colorchecker_bounds
                SOURCES test_colorchecker_bounds.c
                LINK_LIBRARIES lib_darktable cmocka)

add_cmocka_test(test_tonecurve_bounds
                SOURCES test_tonecurve_bounds.c
                LINK_LIBRARIES lib_darktable cmocka)

add_cmocka_test(test_basecurve_bounds
                SOURCES test_basecurve_bounds.c
                LINK_LIBRARIES lib_darktable cmocka)

add_cmocka_test(test_rgbcurve_bounds
                SOURCES test_rgbcurve_bounds.c
                LINK_LIBRARIES lib_darktable cmocka)

add_cmocka_test(test_colorzones_bounds
                SOURCES test_colorzones_bounds.c
                LINK_LIBRARIES lib_darktable cmocka)

add_cmocka_test(test_lut3d_bounds
                SOURCES test_lut3d_bounds.c
                LINK_LIBRARIES lib_darktable cmocka)

add_cmocka_test(test_colorin_strings
                SOURCES test_colorin_strings.c
                LINK_LIBRARIES lib_darktable cmocka)

# Windows: libs have to be copied next to the executable
if(WIN32)
    _copy_required_library(test_filmicrgb lib_darktable)
endif(WIN32)
