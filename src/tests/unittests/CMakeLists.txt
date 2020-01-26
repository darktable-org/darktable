add_cmocka_test(test_sample
                SOURCES test_sample.c
                LINK_LIBRARIES cmocka)

add_cmocka_mock_test(test_filmicrgb
                     SOURCES test_filmicrgb.c
                     LINK_LIBRARIES lib_darktable cmocka
                     MOCKS dt_iop_color_picker_reset)
