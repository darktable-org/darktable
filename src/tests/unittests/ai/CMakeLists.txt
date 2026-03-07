add_cmocka_test(test_ai_backend
                SOURCES test_ai_backend.c dt_stubs.c
                LINK_LIBRARIES darktable_ai cmocka::cmocka)

target_compile_definitions(test_ai_backend PRIVATE
  TEST_MODEL_DIR="${CMAKE_CURRENT_SOURCE_DIR}/models")
