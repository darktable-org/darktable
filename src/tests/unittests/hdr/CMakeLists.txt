add_cmocka_test(test_hdr_transfer
                SOURCES test-hdr-transfer.c
                LINK_LIBRARIES cmocka::cmocka m)
target_include_directories(test_hdr_transfer PRIVATE ${CMAKE_SOURCE_DIR}/src)

# dt_imageio_pq_light_level() lives in lib_darktable
add_cmocka_test(test_hdr_clli
                SOURCES test-hdr-clli.c
                LINK_LIBRARIES lib_darktable cmocka)

if(WIN32)
  _copy_required_library(test_hdr_clli lib_darktable)
endif(WIN32)
