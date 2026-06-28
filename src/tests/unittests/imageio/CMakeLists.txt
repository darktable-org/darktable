add_cmocka_test(test_libraw
                SOURCES test_libraw.c
                LINK_LIBRARIES lib_darktable cmocka)

# Windows: libs have to be copied next to the executable
if(WIN32)
    _copy_required_library(test_libraw lib_darktable)
endif(WIN32)
