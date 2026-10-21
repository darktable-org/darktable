# the tag-name rule behind darktable.metadata.register() only needs a lua
# state, so it is checked here without a database or a running darktable
add_cmocka_test(test_lua_metadata
                SOURCES test_lua_metadata.c
                LINK_LIBRARIES lib_darktable cmocka)

if(WIN32)
    _copy_required_library(test_lua_metadata lib_darktable)
endif(WIN32)
