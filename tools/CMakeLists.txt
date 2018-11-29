CHECK_COMPILER_FLAG_AND_ENABLE_IT(-fno-strict-aliasing)

if (BUILD_NOISE_TOOLS)
    add_subdirectory(noise)
endif()
