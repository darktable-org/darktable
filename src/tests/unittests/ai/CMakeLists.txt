add_cmocka_test(test_ai_backend
                SOURCES test_ai_backend.c dt_stubs.c
                LINK_LIBRARIES darktable_ai cmocka::cmocka)

target_compile_definitions(test_ai_backend PRIVATE
  TEST_MODEL_DIR="${CMAKE_CURRENT_SOURCE_DIR}/models")

# the backend always scans $XDG_DATA_HOME/darktable/models on top of the
# search paths it is given, so point it at an empty directory -- otherwise
# models the developer has installed leak into the discovery assertions
set(_AI_TEST_ENV "XDG_DATA_HOME=${CMAKE_CURRENT_BINARY_DIR}/xdg-data")

# ORT is lazy-loaded by bare filename here, so the build-tree copy has to be
# on the loader search path (installed builds find it via plugindir instead)
if(UNIX AND NOT APPLE AND NOT ONNXRuntime_SYSTEM_PACKAGE)
  list(APPEND _AI_TEST_ENV "LD_LIBRARY_PATH=${ONNXRuntime_LIB_DIR}")
endif()

set_tests_properties(test_ai_backend PROPERTIES ENVIRONMENT "${_AI_TEST_ENV}")
