include(CheckCSourceCompiles)
include(CheckIncludeFile)
include(CheckSymbolExists)
include(CheckStructHasMember)

set(CMAKE_REQUIRED_INCLUDES ${CMAKE_CURRENT_SOURCE_DIR})

check_c_source_compiles(
  "#include <stdio.h>
int main() {
  #include \"src/is_supported_platform.h\"
}"
  IS_SUPPORTED_PLATFORM)
if(NOT IS_SUPPORTED_PLATFORM)
  message(FATAL_ERROR "The target platform is not supported!")
endif()

set(CMAKE_REQUIRED_INCLUDES)

check_include_file(cpuid.h HAVE_CPUID_H)
if(HAVE_CPUID_H)
  check_symbol_exists(__get_cpuid "cpuid.h" HAVE___GET_CPUID)
endif()

check_include_file(execinfo.h HAVE_EXECINFO_H)

if(OpenMP_FOUND)
  set(CMAKE_REQUIRED_FLAGS ${OpenMP_C_FLAGS})
  set(CMAKE_REQUIRED_LIBRARIES ${OpenMP_C_LIBRARIES})
  set(CMAKE_REQUIRED_INCLUDES ${OpenMP_C_INCLUDE_DIRS})
  check_c_source_compiles(
    "#include <omp.h>

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
}"
    HAVE_OMP_FIRSTPRIVATE_WITH_CONST)

  set(CMAKE_REQUIRED_FLAGS)
  set(CMAKE_REQUIRED_LIBRARIES)
  set(CMAKE_REQUIRED_INCLUDES)
endif()

#
# Check for pthread struct members
#
set(CMAKE_REQUIRED_FLAGS ${THREADS_PREFER_PTHREAD_FLAG})
set(CMAKE_REQUIRED_LIBRARIES ${CMAKE_THREAD_LIBS_INIT})

check_struct_has_member(
  "struct __pthread_rwlock_arch_t" "__readers" "pthread.h"
  HAVE_THREAD_RWLOCK_ARCH_T_READERS LANGUAGE C)

check_struct_has_member(
  "struct __pthread_rwlock_arch_t" "__nr_readers" "pthread.h"
  HAVE_THREAD_RWLOCK_ARCH_T_NR_READERS LANGUAGE C)

unset(CMAKE_REQUIRED_FLAGS)
unset(CMAKE_REQUIRED_LIBRARIES)
unset(CMAKE_REQUIRED_INCLUDES)
