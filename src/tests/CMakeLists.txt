add_executable(darktable-test-variables variables.c)
target_link_libraries(darktable-test-variables lib_darktable)

add_subdirectory(unittests)
