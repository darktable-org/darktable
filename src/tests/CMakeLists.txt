ADD_EXECUTABLE(test_cache cache.c)
TARGET_LINK_LIBRARIES(test_cache ${LIBS})
ADD_TEST(Cache test_cache)
