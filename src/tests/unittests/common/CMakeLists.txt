add_cmocka_test(test_spektra_sim
                SOURCES test_spektra_sim.c
                LINK_LIBRARIES lib_darktable cmocka)

# The pack-backed half of the coverage. darktable does not ship a data pack:
# users install one by hand or the module downloads it from
# darktable-org/darktable-spektrafilm. SPEKTRA_TEST_PACK_DIR names a checkout of
# one, either as the directory holding pack.json or as one holding versioned
# subdirectories that do (the newest is used), so a clone of that repository's
# packs/ directory works as is. The SPEKTRA_PACK_DIR environment variable
# overrides it at run time, and a directory that is set but holds no loadable
# pack fails the suite. With neither set, the suite uses the pack darktable
# itself would find in the user data directory, and with none there it exits
# with 77, which ctest reports as skipped.
set(SPEKTRA_TEST_PACK_DIR "" CACHE PATH
    "spektrafilm data pack for test_spektra_pack (empty: darktable's own)")
add_cmocka_test(test_spektra_pack
                SOURCES test_spektra_pack.c
                LINK_LIBRARIES lib_darktable cmocka)

add_cmocka_test(test_draw_curve
                SOURCES test_draw_curve.c
                LINK_LIBRARIES lib_darktable cmocka)

add_cmocka_test(test_fixed_strings
                SOURCES test_fixed_strings.c
                LINK_LIBRARIES lib_darktable cmocka)

add_cmocka_test(test_history_strings
                SOURCES test_history_strings.c
                LINK_LIBRARIES lib_darktable cmocka)

target_compile_definitions(test_spektra_pack PRIVATE
  SPEKTRA_PACK_DIR="${SPEKTRA_TEST_PACK_DIR}")
set_tests_properties(test_spektra_pack PROPERTIES SKIP_RETURN_CODE 77)

# the .dtdata sidecar container, on temp files: no database needed
add_cmocka_test(test_dtdata
                SOURCES test_dtdata.c
                LINK_LIBRARIES lib_darktable cmocka)

# Windows: libs have to be copied next to the executable
if(WIN32)
    _copy_required_library(test_spektra_sim lib_darktable)
    _copy_required_library(test_spektra_pack lib_darktable)
    _copy_required_library(test_draw_curve lib_darktable)
    _copy_required_library(test_fixed_strings lib_darktable)
    _copy_required_library(test_history_strings lib_darktable)
    _copy_required_library(test_dtdata lib_darktable)
endif(WIN32)
