add_cmocka_test(test_utility
                SOURCES test_utility.c
                LINK_LIBRARIES lib_darktable cmocka)

if(WIN32)
    _copy_required_library(test_utility lib_darktable)
endif(WIN32)
