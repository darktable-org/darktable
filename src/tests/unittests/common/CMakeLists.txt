add_cmocka_mock_test(test_imageio
                     SOURCES test_imageio.c
                     LINK_LIBRARIES lib_darktable cmocka)

# Windows: libs have to be copied next to the executable
if(WIN32)
    _copy_required_library(test_imageio lib_darktable)
endif(WIN32)
