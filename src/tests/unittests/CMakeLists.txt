add_subdirectory(iop)

if(USE_AI)
  add_subdirectory(ai)
endif(USE_AI)

add_cmocka_test(test_sample
                SOURCES test_sample.c
                LINK_LIBRARIES cmocka)

# Windows: main-method requires the wrapper provided by lib-darktable
if(WIN32)
    target_link_libraries(test_sample PRIVATE lib_darktable)
    _copy_required_library(test_sample lib_darktable)
endif(WIN32)

# HDR exposure-bracket auto-alignment. Calls the public dt_hdr_alignment_* API
# exported from lib_darktable (which carries the OpenCV-linked objects); the test
# itself adapts at runtime to OpenCV-present / -absent builds.
add_cmocka_test(test_hdr_alignment
                SOURCES test_hdr_alignment.c
                LINK_LIBRARIES lib_darktable cmocka)

if(WIN32)
    _copy_required_library(test_hdr_alignment lib_darktable)
endif(WIN32)

# White-box tests for the internal (static) helpers of the HDR alignment code.
# Following test_filmicrgb.c, the .c is #included directly for static access;
# lib_darktable supplies what it links against (OpenCV seam, glib, ...).
add_cmocka_test(test_hdr_alignment_internal
                SOURCES test_hdr_alignment_internal.c
                LINK_LIBRARIES lib_darktable cmocka)

if(WIN32)
    _copy_required_library(test_hdr_alignment_internal lib_darktable)
endif(WIN32)
