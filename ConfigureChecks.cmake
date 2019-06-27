include(CheckCSourceCompiles)
include(TestBigEndian)

if (OpenMP_FOUND)

set(CMAKE_REQUIRED_FLAGS ${OpenMP_C_FLAGS})
set(CMAKE_REQUIRED_LIBRARIES ${OpenMP_C_LIBRARIES})
check_c_source_compiles("
#include <omp.h>

static void sink(const int x, int a[])
{
#pragma omp parallel for default(none) firstprivate(x) shared(a)
    for(int i = 0; i < 3; i++) {
        a[i] = x + i;
    }
}

int main(void)
{
    int x = 42;
    int a[3] = {0};

    sink(x, a);

    return 0;
}" HAVE_OMP_FIRSTPRIVATE_WITH_CONST)

set(CMAKE_REQUIRED_FLAGS)
set(CMAKE_REQUIRED_LIBRARIES)
endif()

set(CMAKE_REQUIRED_INCLUDES ${CMAKE_CURRENT_SOURCE_DIR})

check_c_source_compiles("#include <stdio.h>
int main() {
  #include \"src/is_supported_platform.h\"
}" IS_SUPPORTED_PLATFORM)
if (IS_SUPPORTED_PLATFORM)
    message(STATUS "Is the target platform supported: ${IS_SUPPORTED_PLATFORM}")
else()
    message(FATAL_ERROR "The target platform is not supported!")
endif()

set(CMAKE_REQUIRED_INCLUDES)

test_big_endian(BIGENDIAN)
if(${BIGENDIAN})
    # we do not really want those.
    # besides, no one probably tried darktable on such systems
    message(FATAL_ERROR "Found big endian system. Bad.")
else()
    message(STATUS "Found little endian system. Good.")
endif(${BIGENDIAN})
