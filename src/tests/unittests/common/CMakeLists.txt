add_cmocka_test(test_spektra_sim
                SOURCES test_spektra_sim.c
                LINK_LIBRARIES lib_darktable cmocka)

# The pack-backed half of the coverage. SPEKTRA_PACK_DIR names the data pack
# darktable ships, either as the directory holding pack.json or as one holding a
# single versioned subdirectory that does -- the suite resolves both, so it does
# not have to be taught the current spektrafilm release. With no pack there,
# every test in it skips: a build machine that cannot answer the question should
# not report the engine as broken.
add_cmocka_test(test_spektra_pack
                SOURCES test_spektra_pack.c
                LINK_LIBRARIES lib_darktable cmocka)

target_compile_definitions(test_spektra_pack PRIVATE
  SPEKTRA_PACK_DIR="${CMAKE_SOURCE_DIR}/data/spektrafilm")

# Windows: libs have to be copied next to the executable
if(WIN32)
    _copy_required_library(test_spektra_sim lib_darktable)
    _copy_required_library(test_spektra_pack lib_darktable)
endif(WIN32)
