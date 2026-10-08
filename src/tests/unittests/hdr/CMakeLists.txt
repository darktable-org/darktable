add_cmocka_test(test_hdr_transfer
                SOURCES test-hdr-transfer.c
                LINK_LIBRARIES cmocka::cmocka m)
target_include_directories(test_hdr_transfer PRIVATE ${CMAKE_SOURCE_DIR}/src)
